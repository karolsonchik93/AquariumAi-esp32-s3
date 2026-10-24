/* =====================================================================
   Аквариумный контроллер на ESP32-S3 (модуль N16R8: 16 МБ флеша, 8 МБ PSRAM)
   =====================================================================
   Возможности:
     1. Светильник: рассвет → день → закат → ночник (гаснет в заданное время)
     2. Реле 1 — помпа          (кнопка на сайте, состояние переживает сброс)
     3. Реле 2 — компрессор     (кнопка на сайте, состояние переживает сброс)
     4. Реле 3 — вентилятор     (авто по температуре + ручной режим)
     5. Реле 4 — подогреватель 75 Вт (термостат с гистерезисом и защитами)
     + два DS18B20 (вода и воздух), часы NTP, веб-сервер с живым интерфейсом,
       история температур в PSRAM, журнал циклов, OTA-обновление по воздуху.

   Что даёт ESP32-S3 по сравнению с MKR WiFi 1010:
     • два ядра: управление аквариумом живёт на своём ядре и не зависит от веб-сервера;
     • 8 МБ PSRAM — история температур на сутки и больше;
     • настоящий mDNS: http://aquarium.local;
     • обновление прошивки по воздуху (ArduinoOTA и загрузка .bin прямо со страницы);
     • Server-Sent Events — интерфейс обновляется мгновенно, без опроса;
     • аппаратный ШИМ LEDC 12 бит / 5 кГц и адресный светодиод состояния.

   Требуемые библиотеки: OneWire (Paul Stoffregen), DallasTemperature (Miles Burton).
   Всё остальное (Wi-Fi, mDNS, Preferences, Update, OTA, LEDC) уже в ядре ESP32.
   ===================================================================== */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <math.h>
#include <string.h>
#include <strings.h>

#if __has_include("esp32-hal-rgb-led.h")
  #include "esp32-hal-rgb-led.h"
  #define HAVE_RGB_LED 1
#else
  #define HAVE_RGB_LED 0
#endif

#include "config.h"
#include "webui.h"

/* ====================== СОВМЕСТИМОСТЬ ЯДЕР ESP32 ======================
   Прошивка собирается и на Arduino-ESP32 2.x, и на 3.x. В 3.x переписали
   API ШИМ (LEDC) и сторожевого таймера (IDF 5), поэтому все различия
   спрятаны в этих обёртках — остальной код от версии ядра не зависит. */
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2      // на всякий случай: старое ядро
#endif

static void pwmInit(uint8_t ch, uint8_t pin, uint32_t freq, uint8_t bits) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  /* 3.x: канал подбирается автоматически по выводу */
  ledcAttach(pin, freq, bits);
#else
  ledcSetup(ch, freq, bits);
  ledcAttachPin(pin, ch);
#endif
}

static void pwmWrite(uint8_t ch, uint8_t pin, uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch;
  ledcWrite(pin, duty);
#else
  (void)pin;
  ledcWrite(ch, duty);
#endif
}

/* адресный светодиод состояния: в 3.x функция называется rgbLedWrite */
static void rgbWrite(uint8_t pin, uint8_t r, uint8_t g, uint8_t b) {
#if !HAVE_RGB_LED
  (void)pin; (void)r; (void)g; (void)b;
#elif ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(pin, r, g, b);
#else
  neopixelWrite(pin, r, g, b);
#endif
}

/* ====================== СЛУЖЕБНЫЕ КОНСТАНТЫ ====================== */
#define SETTINGS_MAGIC    0xA95E
#define SETTINGS_VERSION  1

#define RLY_PUMP  0
#define RLY_AIR   1
#define RLY_FAN   2
#define RLY_HEAT  3
static const uint8_t RELAY_PINS[4] = { PIN_RELAY_PUMP, PIN_RELAY_AIR, PIN_RELAY_FAN, PIN_RELAY_HEAT };
static const char* const RELAY_NAMES[4] = { "помпа", "компрессор", "вентилятор", "нагреватель" };

#define AL_WATER_SENSOR  0x01
#define AL_AIR_SENSOR    0x02
#define AL_OVERHEAT      0x04
#define AL_HEAT_TIMEOUT  0x08
#define AL_NO_TIME       0x10

enum LightPhase { LP_NIGHT = 0, LP_DAWN, LP_DAY, LP_DUSK, LP_MANUAL };
static const char* const PHASE_NAMES[] = { "night", "dawn", "day", "dusk", "manual" };

/* ====================== СТРУКТУРЫ ====================== */
struct Settings {
  uint16_t magic;
  uint8_t  ver;
  uint8_t  ledCh;
  uint8_t  pumpOn, airOn, lightMode;
  uint8_t  onHour, onMin, offHour, offMin;
  uint8_t  nightOffHour, nightOffMin;
  uint8_t  rampMin, daysMask;
  uint8_t  dayPct[3], nightPct[3], manualPct[3];
  uint8_t  waterRom[8];
  uint8_t  reserved[2];
  float    setpoint, hyst, maxWaterTemp, fanOn, fanHyst, heaterPowerW;
};

struct CycleLog {
  uint32_t startEpoch;
  uint16_t durSec;
  int16_t  tStart100, tEnd100, set100;
};

struct HistPoint {
  int16_t t100;
  int16_t a100;
  uint8_t flags;      // бит0 нагрев, 1 вентилятор, 2 помпа, 3 аэратор, 4 свет
};

/* Снимок состояния: заполняется в контуре управления, читается веб-задачей */
struct Telemetry {
  uint32_t seq;
  float    water, air, waterShown;
  bool     waterOk, airOk;
  bool     pump, airOn, fan, heat;
  bool     heaterLocked, fanManual, heatManual;
  uint32_t heaterOnSec;
  float    setpoint, hyst, maxWaterTemp, fanOn, fanHyst, heaterPowerW;
  uint8_t  lightMode, phase;
  uint8_t  ledPct[3], dayPct[3], nightPct[3], manualPct[3];
  uint8_t  onH, onM, offH, offM, noffH, noffM, ramp, days;
  uint16_t nightLenMin;
  uint8_t  alarms;
  char     alarmText[96];
  uint32_t epoch;
  uint8_t  hour, minute, second, day, month, weekday;
  uint16_t year;
  bool     timeFromNtp, timeApprox;
  char     ip[16], gw[16], ssid[33];
  char     wifiSsid[33], wifiStatus[10];
  bool     apMode, staConnected, wifiCustom;
  int32_t  rssi;
  uint32_t uptimeSec, requests, cmds;
  uint32_t freeHeap, psramSize;
  bool     nvmLoaded, otaActive;
  uint16_t sensorCount;
  uint8_t  waterIdx, airIdx;
  float    kwhTotal, kwhToday;
  uint32_t estSec;
  int16_t  lastT100, lastA100;
  uint8_t  lastFlags;
};

struct CmdMsg { char key[20]; char val[28]; };

/* ====================== ГЛОБАЛЬНЫЕ ОБЪЕКТЫ ====================== */
static Settings cfg;
static Telemetry tele;
static HistPoint* hist = nullptr;          // буфер истории в PSRAM
static uint16_t histCount = 0, histNext = 0;
static uint32_t histAt = 0;
static CycleLog cycles[CYCLE_LOG_SIZE];
static uint8_t cycleCount = 0, cycleNext = 0;
static bool cycleActive = false;
static uint32_t cycleStartMs = 0;
static float cycleStartT = NAN;
static bool newHistPoint = false;

static OneWire oneWire(PIN_ONEWIRE);
static DallasTemperature ds(&oneWire);
static float waterTemp = NAN, airTemp = NAN, waterShown = NAN;
static bool waterOk = false, airOk = false;
static uint8_t waterBad = 0, airBad = 0, dsWaterIdx = 0, dsAirIdx = 0xFF;

static Preferences prefs;
static bool settingsFromNvm = false;
static bool cfgDirty = false;
static uint32_t cfgDirtyAt = 0;

static SemaphoreHandle_t stateMux = nullptr;
static QueueHandle_t cmdQueue = nullptr;
static TaskHandle_t webTaskHandle = nullptr;

static uint8_t relayState[4] = { 0, 0, 0, 0 };
static uint32_t heaterOnAt = 0, heaterOffAt = 0;
static bool heaterLocked = false;
static uint32_t heaterLockUntil = 0;
static bool heatManual = false, heatManualState = false;
static uint32_t heatManualUntil = 0;
static bool fanManual = false, fanManualState = false;
static uint8_t alarms = 0;
static char alarmText[96] = "";

static uint32_t heatMsTotal = 0, heatMsToday = 0, lastHeatTick = 0;
static uint8_t lastDay = 255;

static LightPhase lightPhase = LP_NIGHT;
static uint8_t ledPct[3] = { 0, 0, 0 };
static int32_t ledDuty[3] = { -1, -1, -1 };
static uint16_t gammaLut[101];

static uint32_t lastHeartbeat = 0, lastAlarmBits = 0;
static bool serialWasOpen = false;
static bool otaActive = false;
static uint8_t lastWlStatus = 255;
static bool bannerValid = false;
static uint32_t staLostSince = 0;
static bool wifiWasConnected = false;
static uint32_t wifiNextTry = 0, apNextTry = 0;
static char ipStr[16] = "0.0.0.0", gwStr[16] = "0.0.0.0";

/* --- Wi-Fi, который настраивается со страницы и хранится в NVS --- */
static char wifiSsid[33] = WIFI_SSID;      // к какой сети подключаемся
static char wifiPass[64] = WIFI_PASS;
static bool wifiCustom = false;            // данные пришли со страницы, а не из config.h
static char wifiNewSsid[33];               // заявка от веб-задачи
static char wifiNewPass[64];
static volatile bool wifiApplyReq = false;
static volatile bool wifiForgetReq = false;
static uint32_t apDropAt = 0;              // когда гасить точку доступа после успеха
static uint32_t wifiTryingSince = 0;       // с какого момента пробуем новую сеть

static uint32_t httpRequests = 0, httpCmds = 0;
static esp_reset_reason_t resetReason = ESP_RST_UNKNOWN;
static bool timeApprox = false;

/* сеть */
static WiFiServer server(HTTP_PORT);
static WiFiClient sseClient;
static uint32_t sseLast = 0;
static char sseJson[1600];

/* ====================== ПРОТОТИПЫ ====================== */
static float  clampf(float v, float lo, float hi);
static void   fmtF(char* out, size_t n, float v, uint8_t dec);
static void   ipToStr(IPAddress ip, char* out, size_t n);
static const char* resetReasonName(esp_reset_reason_t r);

static void   settingsDefaults();
static bool   settingsLoad();
static void   settingsSaveNow();
static void   settingsTask();

static void   timeBegin();
static void   timeTask();
static bool   ntpSynced();
static void   epochParts(uint32_t epoch, uint8_t& h, uint8_t& mi, uint8_t& s, uint8_t& d,
                         uint8_t& mo, uint16_t& y, uint8_t& wd);

static void   sensorMap();
static void   sensorTask();

static void   relayWrite(uint8_t ch, bool on);
static void   ledWrite(uint8_t ch, uint8_t pct);
static uint16_t nightLenMinutes();
static void   controlHeater();
static void   controlFan();
static void   controlLight();

static void   historyInit();
static void   historyPush();
static uint16_t historySnapshot(HistPoint* out, uint16_t maxN, uint32_t windowSec);

static void   netBegin();
static void   netTask();
static void   startAP();
static void   connectSta();
static void   publishTelemetry();

static void   webTask(void* pv);
static void   httpServe(WiFiClient& c);
static bool   httpReadRequest(WiFiClient& c, char* method, size_t mlen, char* path, size_t plen,
                              char* query, size_t qlen, long* contentLen);
static void   httpSendPage(WiFiClient& c);
static void   httpSendState(WiFiClient& c);
static void   httpSendHistory(WiFiClient& c, uint32_t windowSec);
static void   httpSendScan(WiFiClient& c);
static void   httpHandleCmd(WiFiClient& c, const char* query);
static void   httpHandleOta(WiFiClient& c, long contentLen);
static void   sseBegin(WiFiClient& c);
static void   ssePump(WiFiClient& c);

static void   enqueueCmd(const char* key, const char* val);
static void   applyCommands();
static void   applyCmd(const char* key, const char* val);
static bool   getParam(const char* q, const char* key, char* out, size_t outLen);
static long   paramInt(const char* q, const char* key, long def);
static float  paramFloat(const char* q, const char* key, float def);
static bool   parseHM(const char* s, uint8_t& h, uint8_t& mi);

static int    buildStateJson(char* out, size_t n);
static void   buildAlarmText();
static void   statusLedTask();
static void   cliTask();
static void   cliHelp();
static void   printConfigSummary();
static void   printSensorList();
static void   printHeartbeat();
static void   printNetBanner(const char* why);
static void   logAlarms();

/* ====================== УТИЛИТЫ ====================== */
static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

/* Своё форматирование float: компактный JSON и независимость от libc */
static void fmtF(char* out, size_t n, float v, uint8_t dec) {
  if (isnan(v)) { snprintf(out, n, "null"); return; }
  bool neg = (v < 0);
  if (neg) v = -v;
  uint32_t mul = (dec == 0) ? 1UL : (dec == 1 ? 10UL : 100UL);
  uint32_t x = (uint32_t)(v * (float)mul + 0.5f);
  uint32_t ip = x / mul, fp = x % mul;
  if (dec == 0)      snprintf(out, n, "%s%lu", neg ? "-" : "", (unsigned long)ip);
  else if (dec == 1) snprintf(out, n, "%s%lu.%01lu", neg ? "-" : "", (unsigned long)ip, (unsigned long)fp);
  else               snprintf(out, n, "%s%lu.%02lu", neg ? "-" : "", (unsigned long)ip, (unsigned long)fp);
}

static void ipToStr(IPAddress ip, char* out, size_t n) {
  snprintf(out, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "включение питания";
    case ESP_RST_EXT:      return "внешний сброс (кнопка)";
    case ESP_RST_SW:       return "программная перезагрузка";
    case ESP_RST_PANIC:    return "аварийное завершение (panic)";
    case ESP_RST_INT_WDT:  return "аппаратный watchdog";
    case ESP_RST_TASK_WDT: return "watchdog задачи (зависание)";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "просадка питания";
    case ESP_RST_DEEPSLEEP:return "выход из глубокого сна";
    default:               return "неизвестно";
  }
}

static bool parseHM(const char* s, uint8_t& h, uint8_t& mi) {
  if (!s) return false;
  while (*s == ' ') s++;
  if (*s < '0' || *s > '9') return false;
  int hh = 0, mm = 0;
  if (s[1] >= '0' && s[1] <= '9') { hh = (s[0] - '0') * 10 + (s[1] - '0'); s += 2; }
  else { hh = s[0] - '0'; s += 1; }
  while (*s == ' ' || *s == ':' || *s == '.') s++;
  if (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9') mm = (s[0] - '0') * 10 + (s[1] - '0');
  else if (s[0] >= '0' && s[0] <= '9') mm = s[0] - '0';
  if (hh > 23 || mm > 59) return false;
  h = (uint8_t)hh; mi = (uint8_t)mm;
  return true;
}

/* ====================== НАСТРОЙКИ (NVS) ====================== */
static void settingsDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = SETTINGS_MAGIC;
  cfg.ver = SETTINGS_VERSION;
  cfg.ledCh = LED_CHANNELS;
  cfg.pumpOn = 1; cfg.airOn = 1;
  cfg.lightMode = 0;
  cfg.onHour = DEF_LIGHT_ON_H;   cfg.onMin = DEF_LIGHT_ON_M;
  cfg.offHour = DEF_LIGHT_OFF_H; cfg.offMin = DEF_LIGHT_OFF_M;
  cfg.nightOffHour = DEF_NIGHT_OFF_H; cfg.nightOffMin = DEF_NIGHT_OFF_M;
  cfg.rampMin = DEF_LIGHT_RAMP_MIN;
  cfg.daysMask = DEF_DAYS_MASK;
  cfg.dayPct[0] = DEF_DAY_PCT_1; cfg.dayPct[1] = DEF_DAY_PCT_2; cfg.dayPct[2] = DEF_DAY_PCT_3;
  cfg.nightPct[0] = DEF_NIGHT_PCT_1; cfg.nightPct[1] = DEF_NIGHT_PCT_2; cfg.nightPct[2] = DEF_NIGHT_PCT_3;
  cfg.manualPct[0] = 60; cfg.manualPct[1] = 40; cfg.manualPct[2] = 0;
  cfg.setpoint = DEF_SETPOINT;
  cfg.hyst = DEF_HYST;
  cfg.maxWaterTemp = DEF_MAX_WATER_TEMP;
  cfg.fanOn = DEF_FAN_ON_TEMP;
  cfg.fanHyst = DEF_FAN_HYST;
  cfg.heaterPowerW = DEF_HEATER_POWER_W;
}

static bool settingsLoad() {
  prefs.begin(NVM_NAMESPACE, false);
  Settings tmp;
  size_t got = prefs.getBytes("cfg", &tmp, sizeof(tmp));
  if (got == sizeof(tmp) && tmp.magic == SETTINGS_MAGIC && tmp.ver == SETTINGS_VERSION &&
      tmp.ledCh == LED_CHANNELS) {
    cfg = tmp;
    settingsFromNvm = true;
    return true;
  }
  settingsDefaults();
  return false;
}

static void settingsSaveNow() {
  cfg.magic = SETTINGS_MAGIC;
  cfg.ver = SETTINGS_VERSION;
  cfg.ledCh = LED_CHANNELS;
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  settingsFromNvm = true;
  cfgDirty = false;
}

/* Запись в NVS откладывается: пользователь может крутить слайдеры подряд */
static void settingsTask() {
  static bool pending = false;
  static uint32_t since = 0;
  uint32_t now = millis();
  if (otaActive) return;                       // во время OTA флеш не трогаем
  if (cfgDirty && !pending) { pending = true; since = now; }
  if (pending && (now - since) >= 20000UL) {
    pending = false;
    settingsSaveNow();
    Serial.println(F("[nvs] настройки сохранены"));
  }
}

/* ====================== ВРЕМЯ ====================== */
static void epochParts(uint32_t epoch, uint8_t& h, uint8_t& mi, uint8_t& s, uint8_t& d,
                       uint8_t& mo, uint16_t& y, uint8_t& wd) {
  time_t t = (time_t)epoch;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  h = tmv.tm_hour; mi = tmv.tm_min; s = tmv.tm_sec;
  d = tmv.tm_mday; mo = tmv.tm_mon + 1; y = tmv.tm_year + 1900;
  wd = tmv.tm_wday;                              // 0 = воскресенье
}

static void timeBegin() {
  /* запоминаем время между перезагрузками, чтобы расписание не сбивалось */
  prefs.begin(NVM_NAMESPACE, false);
  uint32_t saved = prefs.getUInt("epoch", 0);
  if (saved > 1700000000UL) {
    struct timeval tv = { (time_t)saved, 0 };
    settimeofday(&tv, nullptr);
    timeApprox = true;
    Serial.println(F("[time] часы восстановлены из памяти (приблизительно, до синхронизации NTP)"));
  } else {
    /* если совсем нечего взять — время компиляции */
    const char* d = __DATE__; const char* t = __TIME__;
    static const char* mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int month = 1;
    for (int i = 0; i < 12; i++) if (strncmp(d, mon + i * 3, 3) == 0) { month = i + 1; break; }
    struct tm tmv = {};
    tmv.tm_year = atoi(d + 7) - 1900;
    tmv.tm_mon = month - 1;
    tmv.tm_mday = atoi(d + 4);
    tmv.tm_hour = atoi(t);
    tmv.tm_min = atoi(t + 3);
    tmv.tm_sec = atoi(t + 6);
    time_t epoch = mktime(&tmv) - (time_t)TZ_OFFSET_MIN * 60;
    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);
    timeApprox = true;
    Serial.println(F("[time] часы выставлены по времени компиляции (NTP ещё не ответил)"));
  }
  configTime((long)TZ_OFFSET_MIN * 60, 0, NTP_SERVER_1, NTP_SERVER_2);
}

static void timeTask() {
  static uint32_t lastSave = 0, lastTry = 0;
  uint32_t now = millis();

  /* если Wi-Fi поднят, раз в 30 секунд пробуем добрать время по NTP */
  if ((WiFi.status() == WL_CONNECTED) && (timeApprox || (now - lastTry) > 600000UL)) {
    if (now - lastTry > 30000UL) {
      lastTry = now;
      struct tm tmv;
      if (getLocalTime(&tmv, 10)) {
        if (tmv.tm_year > 120) {                 // год больше 2020 — время достоверное
          if (timeApprox) Serial.println(F("[time] NTP синхронизирован"));
          timeApprox = false;
        }
      }
    }
  }
  /* раз в 10 минут сохраняем время в NVS — пригодится после перезагрузки */
  if (!otaActive && (now - lastSave) > 600000UL) {
    lastSave = now;
    time_t t = time(nullptr);
    if (t > 1700000000) prefs.putUInt("epoch", (uint32_t)t);
  }
}

/* ====================== ДАТЧИКИ ====================== */
static void sensorMap() {
  uint8_t n = ds.getDeviceCount();
  dsWaterIdx = 0; dsAirIdx = 0xFF;
  if (n == 0) return;
  bool romSet = false;
  for (uint8_t i = 0; i < 8; i++) if (cfg.waterRom[i]) { romSet = true; break; }
  if (romSet) {
    DeviceAddress a;
    for (uint8_t i = 0; i < n; i++) {
      if (ds.getAddress(a, i) && memcmp(a, cfg.waterRom, 8) == 0) { dsWaterIdx = i; break; }
    }
  }
  for (uint8_t i = 0; i < n; i++) if (i != dsWaterIdx) { dsAirIdx = i; break; }
}

static void sensorTask() {
  static uint8_t state = 0;
  static uint32_t t0 = 0, lastReq = 0;
  uint32_t now = millis();

  if (state == 0) {
    if (now - lastReq >= SENSOR_PERIOD_MS) {
      ds.requestTemperatures();
      state = 1; t0 = now;
    }
    return;
  }
  if (now - t0 < SENSOR_CONV_MS) return;
  state = 0; lastReq = now;

  float tw = ds.getTempCByIndex(dsWaterIdx);
  bool wOk = (tw > TEMP_MIN_VALID && tw < TEMP_MAX_VALID && tw != 85.0f);
  if (wOk) {
    waterTemp = tw; waterBad = 0; waterOk = true;
    waterShown = isnan(waterShown) ? tw : (waterShown + 0.35f * (tw - waterShown));
    alarms &= (uint8_t)~AL_WATER_SENSOR;
  } else if (waterBad < 250) {
    waterBad++;
    if (waterBad >= SENSOR_FAIL_COUNT) waterOk = false, alarms |= AL_WATER_SENSOR;
  }

  if (dsAirIdx != 0xFF) {
    float ta = ds.getTempCByIndex(dsAirIdx);
    bool aOk = (ta > TEMP_MIN_VALID && ta < TEMP_MAX_VALID && ta != 85.0f);
    if (aOk) { airTemp = ta; airBad = 0; airOk = true; alarms &= (uint8_t)~AL_AIR_SENSOR; }
    else if (airBad < 250) { airBad++; if (airBad >= SENSOR_FAIL_COUNT) airOk = false, alarms |= AL_AIR_SENSOR; }
  } else {
    airOk = false;
    alarms |= AL_AIR_SENSOR;
  }
}

/* ====================== ИСПОЛНИТЕЛЬНЫЕ УСТРОЙСТВА ====================== */
static void relayWrite(uint8_t ch, bool on) {
  if (ch > 3) return;
  if (relayState[ch] == on) return;
#if RELAY_ACTIVE_LOW
  digitalWrite(RELAY_PINS[ch], on ? LOW : HIGH);
#else
  digitalWrite(RELAY_PINS[ch], on ? HIGH : LOW);
#endif
  relayState[ch] = on;

  char b[16];
  Serial.print(F("[реле] канал ")); Serial.print((unsigned)(ch + 1));
  Serial.print(' '); Serial.print(RELAY_NAMES[ch]);
  Serial.print(on ? F(" ВКЛ") : F(" выкл"));
  if (ch == RLY_HEAT && waterOk) {
    Serial.print(F("  (вода ")); fmtF(b, sizeof(b), waterTemp, 2);
    Serial.print(b); Serial.print(F(" °C, уставка "));
    fmtF(b, sizeof(b), cfg.setpoint, 1); Serial.print(b); Serial.print(F(" °C)"));
  } else if (ch == RLY_FAN && waterOk) {
    Serial.print(F("  (вода ")); fmtF(b, sizeof(b), waterTemp, 2); Serial.print(b); Serial.print(F(" °C)"));
  }
  Serial.println();
}

/* ШИМ через аппаратный LEDC: 12 бит, 5 кГц */
static void ledWrite(uint8_t ch, uint8_t pct) {
  if (ch >= LED_CHANNELS) return;
  if (pct > 100) pct = 100;
  ledPct[ch] = pct;
  int32_t duty = gammaLut[pct];
  if (duty == ledDuty[ch]) return;
  ledDuty[ch] = duty;
  pwmWrite(ch, LED_PINS[ch], (uint32_t)duty);
}

static uint16_t nightLenMinutes() {
  int16_t n = (int16_t)((int16_t)cfg.nightOffHour * 60 + cfg.nightOffMin) -
              (int16_t)((int16_t)cfg.offHour * 60 + cfg.offMin);
  n %= 1440;
  if (n < 0) n += 1440;
  return (uint16_t)n;
}

static float smoothstep01(float k) {
  k = clampf(k, 0.0f, 1.0f);
  return k * k * (3.0f - 2.0f * k);
}

static uint32_t estimateHeaterSeconds(float dtC) {
  float J = 200000.0f * clampf(dtC, 0.0f, 10.0f);   // ≈50 л воды
  float P = cfg.heaterPowerW > 1.0f ? cfg.heaterPowerW : 75.0f;
  return (uint32_t)(J / P);
}

/* ====================== РЕГУЛЯТОРЫ ====================== */
static void controlHeater() {
  uint32_t now = millis();
  bool want = relayState[RLY_HEAT];
  bool decided = false;

  if (heatManual) {
    if ((int32_t)(now - heatManualUntil) >= 0) {
      heatManual = false; heatManualState = false;
    } else {
      want = heatManualState;
      if (waterOk && waterTemp > cfg.maxWaterTemp) { want = false; alarms |= AL_OVERHEAT; }
      decided = true;
    }
  }

  if (!decided) {
    if (!waterOk) {
      want = false;
    } else if (waterTemp > cfg.maxWaterTemp) {
      want = false;
      alarms |= AL_OVERHEAT;
      if (!heaterLocked) Serial.println(F("[heat] ПЕРЕГРЕВ: нагрев выключен"));
      heaterLocked = true;
      heaterLockUntil = now + HEATER_LOCK_MS;
    } else {
      if ((alarms & AL_OVERHEAT) && waterTemp < (cfg.maxWaterTemp - 1.5f))
        alarms &= (uint8_t)~AL_OVERHEAT;
      if (heaterLocked && (int32_t)(now - heaterLockUntil) < 0) {
        want = false;
      } else {
        heaterLocked = false;
        if (relayState[RLY_HEAT]) want = (waterTemp < cfg.setpoint + cfg.hyst);
        else                      want = (waterTemp < cfg.setpoint - cfg.hyst);
        if (want && !relayState[RLY_HEAT] && (now - heaterOffAt) < MIN_HEATER_OFF_MS) want = false;
        if (!want && relayState[RLY_HEAT] && (now - heaterOnAt) < MIN_HEATER_ON_MS) want = true;
        if (relayState[RLY_HEAT] && (now - heaterOnAt) > MAX_HEATER_ON_MS) {
          want = false;
          alarms |= AL_HEAT_TIMEOUT;
          heaterLocked = true;
          heaterLockUntil = now + HEATER_LOCK_MS;
          Serial.println(F("[heat] слишком долгий нагрев: аварийное отключение"));
        }
      }
    }
  }

  if (want != relayState[RLY_HEAT]) {
    relayWrite(RLY_HEAT, want);
    if (want) {
      heaterOnAt = now; cycleActive = true; cycleStartMs = now;
      cycleStartT = waterOk ? waterTemp : NAN;
    } else {
      heaterOffAt = now;
      if (cycleActive) {
        cycleActive = false;
        CycleLog& cl = cycles[cycleNext];
        cl.startEpoch = (uint32_t)time(nullptr) - (now - cycleStartMs) / 1000UL;
        cl.durSec = (uint16_t)((now - cycleStartMs) / 1000UL);
        cl.tStart100 = (int16_t)lroundf(cycleStartT * 100.0f);
        cl.tEnd100 = waterOk ? (int16_t)lroundf(waterTemp * 100.0f) : 0;
        cl.set100 = (int16_t)lroundf(cfg.setpoint * 100.0f);
        cycleNext = (uint8_t)((cycleNext + 1) % CYCLE_LOG_SIZE);
        if (cycleCount < CYCLE_LOG_SIZE) cycleCount++;
      }
    }
  }
#if FAN_PWM_ENABLE
  /* плавная скорость вентилятора по температуре */
  if (relayState[RLY_FAN]) {
    float over = waterTemp - cfg.fanOn;
    uint8_t pct = (uint8_t)clampf(40.0f + over * 40.0f, 40.0f, 100.0f);
    static uint8_t lastPct = 255;
    if (pct != lastPct) { lastPct = pct; pwmWrite(3, PIN_FAN_PWM, gammaLut[pct]); }
  } else {
    pwmWrite(3, PIN_FAN_PWM, 0);
  }
#endif
}

static void controlFan() {
  if (fanManual) { relayWrite(RLY_FAN, fanManualState); return; }
  if (!waterOk) { relayWrite(RLY_FAN, false); return; }
  bool on = relayState[RLY_FAN];
  if (on) on = (waterTemp > (cfg.fanOn - cfg.fanHyst));
  else    on = (waterTemp > (cfg.fanOn + cfg.fanHyst));
  if (alarms & AL_OVERHEAT) on = true;
  relayWrite(RLY_FAN, on);
}

static void controlLight() {
  uint32_t now = millis();
  static uint32_t lastRun = 0;
  if (now - lastRun < 500) return;
  lastRun = now;

  if (cfg.lightMode == 2) {                     // ночник принудительно
    lightPhase = LP_NIGHT;
    for (uint8_t i = 0; i < LED_CHANNELS; i++) ledWrite(i, cfg.nightPct[i]);
    return;
  }
  if (cfg.lightMode == 1) {                     // ручной режим
    lightPhase = LP_MANUAL;
    for (uint8_t i = 0; i < LED_CHANNELS; i++) ledWrite(i, cfg.manualPct[i]);
    return;
  }

  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  bool dayEnabled = ((cfg.daysMask >> tmv.tm_wday) & 0x01) != 0;

  int16_t nowMin = (int16_t)tmv.tm_hour * 60 + tmv.tm_min;
  int16_t onMin  = (int16_t)cfg.onHour * 60 + cfg.onMin;
  int16_t offMin = (int16_t)cfg.offHour * 60 + cfg.offMin;
  int16_t dur = (int16_t)(offMin - onMin);
  if (dur <= 0) dur += 1440;
  int16_t el = (int16_t)(nowMin - onMin);
  if (el < 0) el += 1440;

  uint8_t pct[LED_CHANNELS];
  uint8_t ramp = cfg.rampMin < 1 ? 1 : cfg.rampMin;
  int16_t nightLen = (int16_t)nightLenMinutes();

  if (!dayEnabled) {
    lightPhase = LP_NIGHT;
    for (uint8_t i = 0; i < LED_CHANNELS; i++) pct[i] = 0;
  } else if (el >= dur) {
    int16_t afterOff = (int16_t)(el - dur);
    lightPhase = LP_NIGHT;
    for (uint8_t i = 0; i < LED_CHANNELS; i++)
      pct[i] = (nightLen > 0 && afterOff < nightLen) ? cfg.nightPct[i] : 0;
  } else if (el < ramp) {
    lightPhase = LP_DAWN;
    float s = smoothstep01((float)el / (float)ramp);
    for (uint8_t i = 0; i < LED_CHANNELS; i++) pct[i] = (uint8_t)((float)cfg.dayPct[i] * s + 0.5f);
  } else if (el >= (int16_t)(dur - ramp)) {
    lightPhase = LP_DUSK;
    float s = smoothstep01((float)(dur - el) / (float)ramp);
    for (uint8_t i = 0; i < LED_CHANNELS; i++) pct[i] = (uint8_t)((float)cfg.dayPct[i] * s + 0.5f);
  } else {
    lightPhase = LP_DAY;
    for (uint8_t i = 0; i < LED_CHANNELS; i++) pct[i] = cfg.dayPct[i];
  }
  for (uint8_t i = 0; i < LED_CHANNELS; i++) ledWrite(i, pct[i]);
}

/* ====================== ИСТОРИЯ (PSRAM) ====================== */
static void historyInit() {
  size_t bytes = sizeof(HistPoint) * HISTORY_MAX_POINTS;
  if (psramFound()) {
    hist = (HistPoint*)ps_malloc(bytes);
    Serial.print(F("[psram] найдена, размер "));
    Serial.print((unsigned)(ESP.getPsramSize() / 1024));
    Serial.println(F(" КБ"));
  }
  if (!hist) hist = (HistPoint*)malloc(bytes);      // запасной вариант — обычная память
  if (hist) {
    memset(hist, 0, bytes);
    Serial.print(F("[hist] буфер истории "));
    Serial.print((unsigned)(bytes / 1024));
    Serial.print(F(" КБ на "));
    Serial.print((unsigned)HISTORY_MAX_POINTS);
    Serial.println(F(" точек"));
  }
}

static void historyPush() {
  if (!hist) return;
  HistPoint& p = hist[histNext];
  p.t100 = waterOk ? (int16_t)lroundf(waterTemp * 100.0f) : 0;
  p.a100 = airOk ? (int16_t)lroundf(airTemp * 100.0f) : 0;
  p.flags = 0;
  if (relayState[RLY_HEAT]) p.flags |= 0x01;
  if (relayState[RLY_FAN])  p.flags |= 0x02;
  if (relayState[RLY_PUMP]) p.flags |= 0x04;
  if (relayState[RLY_AIR])  p.flags |= 0x08;
  for (uint8_t i = 0; i < LED_CHANNELS; i++) if (ledPct[i] > 2) { p.flags |= 0x10; break; }

  histNext = (uint16_t)((histNext + 1) % HISTORY_MAX_POINTS);
  if (histCount < HISTORY_MAX_POINTS) histCount++;
  newHistPoint = true;
}

/* отдаём не больше maxN точек, прореживая окно */
static uint16_t historySnapshot(HistPoint* out, uint16_t maxN, uint32_t windowSec) {
  if (!hist || histCount == 0) return 0;
  uint32_t wantPoints = windowSec / HISTORY_PERIOD_S;
  if (wantPoints == 0) wantPoints = 1;
  if (wantPoints > histCount) wantPoints = histCount;
  uint16_t step = (uint16_t)(wantPoints / maxN);
  if (step < 1) step = 1;
  uint16_t n = 0;
  uint16_t start = (uint16_t)((histNext + HISTORY_MAX_POINTS - (uint16_t)wantPoints) % HISTORY_MAX_POINTS);
  for (uint32_t k = 0; k < wantPoints && n < maxN; k += step) {
    out[n++] = hist[(start + k) % HISTORY_MAX_POINTS];
  }
  return n;
}

/* ====================== СЕТЬ ====================== */
static const char* wlStatusName(uint8_t st) {
  switch (st) {
    case WL_IDLE_STATUS:     return "ожидание";
    case WL_NO_SSID_AVAIL:   return "сеть не найдена";
    case WL_SCAN_COMPLETED:  return "сканирование";
    case WL_CONNECTED:       return "подключено";
    case WL_CONNECT_FAILED:  return "ошибка подключения (проверьте пароль)";
    case WL_CONNECTION_LOST: return "связь потеряна";
    case WL_DISCONNECTED:    return "не подключено";
    default:                 return "неизвестный статус";
  }
}

static void printNetBanner(const char* why) {
  Serial.println();
  Serial.println(F("============================================================"));
  Serial.print(F("  ВЕБ-СЕРВЕР РАБОТАЕТ — ")); Serial.println(why);
  Serial.println(F("------------------------------------------------------------"));
  Serial.print(F("  Откройте в браузере:   http://")); Serial.println(ipStr);
  if (tele.apMode) {
    Serial.print(F("  точка доступа:         ")); Serial.print(AP_SSID);
    Serial.print(F("  пароль: ")); Serial.println(AP_PASS);
    Serial.println(F("  адрес точки доступа всегда 192.168.4.1"));
  }
#if ENABLE_MDNS
  Serial.print(F("  или по имени:          http://")); Serial.print(WIFI_HOSTNAME); Serial.println(F(".local"));
#endif
#if ENABLE_OTA
  Serial.println(F("  обновление прошивки:   по воздуху (ArduinoOTA) или через страницу"));
#endif
  Serial.println(F("============================================================"));
  Serial.println();
}

/* --- данные сети: берём из NVS, если их туда положили со страницы --- */
static void wifiLoadCreds() {
  snprintf(wifiSsid, sizeof(wifiSsid), "%s", WIFI_SSID);
  snprintf(wifiPass, sizeof(wifiPass), "%s", WIFI_PASS);
  wifiCustom = false;
  String s = prefs.getString("wssid", "");
  if (s.length() > 0) {
    snprintf(wifiSsid, sizeof(wifiSsid), "%s", s.c_str());
    snprintf(wifiPass, sizeof(wifiPass), "%s", prefs.getString("wpass", "").c_str());
    wifiCustom = true;
  }
}

static void wifiSaveCreds(const char* ssid, const char* pass) {
  if (!ssid || !*ssid) return;
  prefs.putString("wssid", ssid);
  prefs.putString("wpass", pass ? pass : "");
  snprintf(wifiSsid, sizeof(wifiSsid), "%s", ssid);
  snprintf(wifiPass, sizeof(wifiPass), "%s", pass ? pass : "");
  wifiCustom = true;
  apDropAt = 0;
}

static void wifiForgetCreds() {
  prefs.remove("wssid");
  prefs.remove("wpass");
  snprintf(wifiSsid, sizeof(wifiSsid), "%s", WIFI_SSID);
  snprintf(wifiPass, sizeof(wifiPass), "%s", WIFI_PASS);
  wifiCustom = false;
  apDropAt = 0;
}

/* короткий код состояния для страницы: ok / wait / nosid / badpass / lost */
static const char* wifiStatusKey() {
  uint8_t st = WiFi.status();
  if (st == WL_CONNECTED) return "ok";
  if (st == WL_NO_SSID_AVAIL) return "nosid";
  if (st == WL_CONNECT_FAILED) return "badpass";
  if (st == WL_CONNECTION_LOST) return "lost";
  return "wait";
}

static void connectSta() {
#if USE_STATIC_IP
  IPAddress ip(STATIC_IP), gw(STATIC_GW), mask(STATIC_MASK), dns(STATIC_DNS);
  WiFi.config(ip, gw, mask, dns);
#endif
  WiFi.setHostname(WIFI_HOSTNAME);
  Serial.print(F("[wifi] подключаюсь к сети \"")); Serial.print(wifiSsid);
  Serial.print(F("\""));
  if (wifiCustom) Serial.print(F(" (данные со страницы)"));
  Serial.println(F(" ..."));
  WiFi.begin(wifiSsid, wifiPass);
}

static void startAP() {
  Serial.println();
  Serial.print(F("[wifi] поднимаю точку доступа \"")); Serial.print(AP_SSID); Serial.println(F("\""));
  /* Сначала аккуратно прекращаем попытку подключения: иначе ESP-IDF
     пишет в лог «sta is connecting, cannot set config» при смене режима. */
  if (WiFi.status() == WL_IDLE_STATUS || WiFi.status() == WL_DISCONNECTED ||
      WiFi.status() == WL_NO_SSID_AVAIL || WiFi.status() == WL_CONNECT_FAILED) {
    WiFi.disconnect(false, false);
  }
  WiFi.mode(WIFI_AP_STA);                  // AP и попытки подключиться к роутеру — одновременно
  WiFi.softAP(AP_SSID, AP_PASS);
  tele.apMode = true;
  IPAddress aip = WiFi.softAPIP();
  ipToStr(aip, ipStr, sizeof(ipStr));
  apNextTry = millis() + 60000UL;
  Serial.print(F("[wifi] подключитесь к Wi-Fi \"")); Serial.print(AP_SSID);
  Serial.print(F("\" (пароль ")); Serial.print(AP_PASS); Serial.print(F("), адрес http://")); Serial.println(ipStr);
  printNetBanner("режим точки доступа");
  bannerValid = true;
}

static void netBegin() {
  wifiLoadCreds();                         // сеть из NVS (если настроена со страницы)
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                    // веб-интерфейс отзывается быстрее
  connectSta();
  wifiNextTry = millis() + WIFI_RETRY_MS;
}

static void netTask() {
  static uint32_t lastCheck = 0;
  uint32_t now = millis();
  if (now - lastCheck < 1000) return;
  lastCheck = now;

  /* ---- заявки со страницы: сменить сеть или забыть её ---- */
  if (wifiForgetReq) {
    wifiForgetReq = false;
    wifiForgetCreds();
    Serial.println(F("[wifi] сохранённая сеть удалена, возвращаюсь к данным из config.h"));
    bannerValid = false;
    wifiWasConnected = false;
    staLostSince = 0;
    WiFi.disconnect();
    connectSta();
    wifiTryingSince = now;
    wifiNextTry = now + WIFI_RETRY_MS;
  }
  if (wifiApplyReq) {
    wifiApplyReq = false;
    wifiSaveCreds(wifiNewSsid, wifiNewPass);
    Serial.print(F("[wifi] сохранил сеть \"")); Serial.print(wifiSsid);
    Serial.println(F("\" в память, переподключаюсь"));
    bannerValid = false;
    wifiWasConnected = false;
    staLostSince = 0;
    WiFi.disconnect();
    connectSta();
    wifiTryingSince = now;
    wifiNextTry = now + WIFI_RETRY_MS;
    Serial.println(F("[wifi] если сеть не ответит за 45 с — останется точка доступа"));
  }

  uint8_t st = WiFi.status();
  tele.staConnected = (st == WL_CONNECTED);

  if (st == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    ipToStr(ip, ipStr, sizeof(ipStr));
    ipToStr(WiFi.gatewayIP(), gwStr, sizeof(gwStr));
    tele.rssi = WiFi.RSSI();
    wifiWasConnected = true;
    staLostSince = 0;
    wifiTryingSince = 0;
    /* имя сети: иногда WiFi.SSID() пуст сразу после подключения — берём сохранённое */
    String ssidNow = WiFi.SSID();
    if (!ssidNow.length()) ssidNow = wifiSsid;
    if (!bannerValid) {
      Serial.print(F("[wifi] подключено к \"")); Serial.print(ssidNow);
      Serial.print(F("\", IP ")); Serial.print(ipStr);
      Serial.print(F(", RSSI ")); Serial.print((long)tele.rssi); Serial.println(F(" dBm"));
#if ENABLE_MDNS
      if (MDNS.begin(WIFI_HOSTNAME)) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        Serial.print(F("[mdns] имя в сети: http://")); Serial.print(WIFI_HOSTNAME); Serial.println(F(".local"));
      }
#endif
      printNetBanner("подключение к роутеру выполнено");
      bannerValid = true;
    }
    /* вышли в сеть — точку доступа держим ещё 30 секунд, чтобы страница показала новый адрес */
    if (tele.apMode) {
      if (apDropAt == 0) {
        apDropAt = now + 30000UL;
        Serial.println(F("[wifi] сеть найдена — точка доступа выключится через 30 секунд"));
      } else if ((int32_t)(now - apDropAt) >= 0) {
        WiFi.softAPdisconnect(true);
        tele.apMode = false;
        apDropAt = 0;
        Serial.println(F("[wifi] точка доступа выключена, плата работает в вашей сети"));
      }
    }
    lastWlStatus = st;
    return;
  }

  if (st != lastWlStatus) {
    Serial.print(F("[wifi] статус: ")); Serial.print(wlStatusName(st));
    if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL || st == WL_CONNECTION_LOST) {
      Serial.print(F(" (код ")); Serial.print((unsigned)st); Serial.print(')');
    }
    Serial.println();
    lastWlStatus = st;
    bannerValid = false;
  }

  /* новая сеть не ответила за 45 с — возвращаем точку доступа, чтобы не потерять доступ */
  if (wifiTryingSince && (now - wifiTryingSince) > 45000UL) {
    wifiTryingSince = 0;
    if (!tele.apMode) {
      Serial.println(F("[wifi] новая сеть не ответила — поднимаю точку доступа для настройки"));
      startAP();
      return;
    }
  }

  if (wifiWasConnected) {
    if (staLostSince == 0) staLostSince = now;
#if AP_FALLBACK
    if (!tele.apMode && (now - staLostSince) > AP_ON_LOST_MS) {
      Serial.println(F("[wifi] связи нет больше 5 минут — поднимаю точку доступа"));
      startAP();
      return;
    }
#endif
  }

  if (now < wifiNextTry) return;
  wifiNextTry = now + WIFI_RETRY_MS;
  if (!wifiWasConnected && !tele.apMode && now > STA_TIMEOUT_MS) {
#if AP_FALLBACK
    if (strcmp(WIFI_SSID, WIFI_SSID_DEFAULT) == 0) {
      Serial.println(F("[wifi] в config.h оставлена сеть-заглушка — перехожу в точку доступа"));
      startAP();
      return;
    }
    Serial.println(F("[wifi] роутер не отвечает — поднимаю точку доступа"));
    startAP();
    return;
#endif
  }
  /* повторная попытка: не дёргаем begin(), пока идёт предыдущее подключение */
  if (!tele.apMode && (st == WL_DISCONNECTED || st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED))
    connectSta();
}

/* ====================== ПУБЛИКАЦИЯ СОСТОЯНИЯ ====================== */
static void publishTelemetry() {
  if (xSemaphoreTake(stateMux, pdMS_TO_TICKS(50)) != pdTRUE) return;
  tele.seq++;
  tele.water = waterOk ? waterTemp : NAN;
  tele.air = airOk ? airTemp : NAN;
  tele.waterShown = waterShown;
  tele.waterOk = waterOk;
  tele.airOk = airOk;
  tele.pump = relayState[RLY_PUMP];
  tele.airOn = relayState[RLY_AIR];
  tele.fan = relayState[RLY_FAN];
  tele.heat = relayState[RLY_HEAT];
  tele.heaterLocked = heaterLocked;
  tele.fanManual = fanManual;
  tele.heatManual = heatManual;
  tele.heaterOnSec = relayState[RLY_HEAT] ? (millis() - heaterOnAt) / 1000UL : 0;
  tele.setpoint = cfg.setpoint;
  tele.hyst = cfg.hyst;
  tele.maxWaterTemp = cfg.maxWaterTemp;
  tele.fanOn = cfg.fanOn;
  tele.fanHyst = cfg.fanHyst;
  tele.heaterPowerW = cfg.heaterPowerW;
  tele.lightMode = cfg.lightMode;
  tele.phase = (uint8_t)lightPhase;
  for (uint8_t i = 0; i < 3; i++) {
    tele.ledPct[i] = ledPct[i];
    tele.dayPct[i] = cfg.dayPct[i];
    tele.nightPct[i] = cfg.nightPct[i];
    tele.manualPct[i] = cfg.manualPct[i];
  }
  tele.onH = cfg.onHour; tele.onM = cfg.onMin;
  tele.offH = cfg.offHour; tele.offM = cfg.offMin;
  tele.noffH = cfg.nightOffHour; tele.noffM = cfg.nightOffMin;
  tele.ramp = cfg.rampMin; tele.days = cfg.daysMask;
  tele.nightLenMin = nightLenMinutes();
  tele.alarms = alarms;
  strncpy(tele.alarmText, alarmText, sizeof(tele.alarmText) - 1);
  tele.alarmText[sizeof(tele.alarmText) - 1] = 0;

  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  tele.epoch = (uint32_t)t;
  tele.hour = tmv.tm_hour; tele.minute = tmv.tm_min; tele.second = tmv.tm_sec;
  tele.day = tmv.tm_mday; tele.month = tmv.tm_mon + 1; tele.year = tmv.tm_year + 1900;
  tele.weekday = tmv.tm_wday;
  tele.timeFromNtp = !timeApprox;
  tele.timeApprox = timeApprox;

  strncpy(tele.ip, ipStr, sizeof(tele.ip) - 1); tele.ip[sizeof(tele.ip) - 1] = 0;
  strncpy(tele.gw, gwStr, sizeof(tele.gw) - 1); tele.gw[sizeof(tele.gw) - 1] = 0;
  String s = WiFi.SSID();
  strncpy(tele.ssid, s.length() ? s.c_str() : (tele.apMode ? AP_SSID : wifiSsid), sizeof(tele.ssid) - 1);
  tele.ssid[sizeof(tele.ssid) - 1] = 0;
  strncpy(tele.wifiSsid, wifiSsid, sizeof(tele.wifiSsid) - 1);
  tele.wifiSsid[sizeof(tele.wifiSsid) - 1] = 0;
  strncpy(tele.wifiStatus, wifiStatusKey(), sizeof(tele.wifiStatus) - 1);
  tele.wifiStatus[sizeof(tele.wifiStatus) - 1] = 0;
  tele.wifiCustom = wifiCustom;

  tele.uptimeSec = millis() / 1000UL;
  tele.requests = httpRequests;
  tele.cmds = httpCmds;
  tele.freeHeap = ESP.getFreeHeap();
  tele.psramSize = ESP.getPsramSize();
  tele.nvmLoaded = settingsFromNvm;
  tele.otaActive = otaActive;
  tele.sensorCount = ds.getDeviceCount();
  tele.waterIdx = dsWaterIdx;
  tele.airIdx = dsAirIdx;
  tele.kwhTotal = cfg.heaterPowerW * (float)heatMsTotal / 3600000.0f;
  tele.kwhToday = cfg.heaterPowerW * (float)heatMsToday / 3600000.0f;
  tele.estSec = waterOk ? estimateHeaterSeconds(cfg.setpoint - waterTemp) : 0;
  xSemaphoreGive(stateMux);
}

/* ====================== JSON ====================== */
static int buildStateJson(char* out, size_t n) {
  Telemetry t;
  if (xSemaphoreTake(stateMux, pdMS_TO_TICKS(100)) == pdTRUE) {
    t = tele;
    xSemaphoreGive(stateMux);
  } else {
    memset(&t, 0, sizeof(t));
  }
  char b[64], s1[24], s2[24], s3[24];
  int w = 0;
  w += snprintf(out + w, n - w, "{\"ok\":1,\"fw\":\"" FW_VERSION "\",\"name\":\"" FW_NAME "\",\"seq\":%lu",
                (unsigned long)t.seq);
  w += snprintf(out + w, n - w, ",\"up\":%lu,\"heap\":%lu,\"psram\":%lu",
                (unsigned long)t.uptimeSec, (unsigned long)t.freeHeap, (unsigned long)t.psramSize);
  fmtF(s1, sizeof(s1), t.water, 2); fmtF(s2, sizeof(s2), t.air, 2); fmtF(s3, sizeof(s3), t.waterShown, 2);
  w += snprintf(out + w, n - w, ",\"t\":%s,\"ta\":%s,\"tf\":%s,\"tok\":%u,\"aok\":%u,\"nsen\":%u",
                s1, s2, s3, t.waterOk ? 1 : 0, t.airOk ? 1 : 0, (unsigned)t.sensorCount);
  w += snprintf(out + w, n - w, ",\"pump\":%u,\"air\":%u,\"fan\":%u,\"heat\":%u,\"hlock\":%u,\"hl\":%lu",
                t.pump ? 1 : 0, t.airOn ? 1 : 0, t.fan ? 1 : 0, t.heat ? 1 : 0,
                t.heaterLocked ? 1 : 0, (unsigned long)t.heaterOnSec);
  w += snprintf(out + w, n - w, ",\"fanman\":%u,\"heatman\":%u", t.fanManual ? 1 : 0, t.heatManual ? 1 : 0);
  fmtF(s1, sizeof(s1), t.setpoint, 1); fmtF(s2, sizeof(s2), t.hyst, 2); fmtF(s3, sizeof(s3), t.maxWaterTemp, 1);
  w += snprintf(out + w, n - w, ",\"sp\":%s,\"hy\":%s,\"maxt\":%s", s1, s2, s3);
  fmtF(s1, sizeof(s1), t.fanOn, 1); fmtF(s2, sizeof(s2), t.fanHyst, 2); fmtF(s3, sizeof(s3), t.heaterPowerW, 0);
  w += snprintf(out + w, n - w, ",\"fanon\":%s,\"fanhy\":%s,\"pw\":%s", s1, s2, s3);
  w += snprintf(out + w, n - w, ",\"kwh\":%lu,\"kwhd\":%lu,\"est\":%lu",
                (unsigned long)(t.kwhTotal * 100.0f), (unsigned long)(t.kwhToday * 100.0f),
                (unsigned long)t.estSec);
  w += snprintf(out + w, n - w, ",\"lmod\":%u,\"phase\":\"%s\",\"lp\":[", (unsigned)t.lightMode,
                PHASE_NAMES[t.phase <= 4 ? t.phase : 0]);
  for (uint8_t i = 0; i < LED_CHANNELS; i++) w += snprintf(out + w, n - w, "%s%u", i ? "," : "", (unsigned)t.ledPct[i]);
  w += snprintf(out + w, n - w, "],\"day\":[");
  for (uint8_t i = 0; i < LED_CHANNELS; i++) w += snprintf(out + w, n - w, "%s%u", i ? "," : "", (unsigned)t.dayPct[i]);
  w += snprintf(out + w, n - w, "],\"night\":[");
  for (uint8_t i = 0; i < LED_CHANNELS; i++) w += snprintf(out + w, n - w, "%s%u", i ? "," : "", (unsigned)t.nightPct[i]);
  w += snprintf(out + w, n - w, "],\"man\":[");
  for (uint8_t i = 0; i < LED_CHANNELS; i++) w += snprintf(out + w, n - w, "%s%u", i ? "," : "", (unsigned)t.manualPct[i]);
  w += snprintf(out + w, n - w, "],\"labels\":[");
  for (uint8_t i = 0; i < LED_CHANNELS; i++) w += snprintf(out + w, n - w, "%s\"%s\"", i ? "," : "", LED_LABELS[i]);
  w += snprintf(out + w, n - w, "],\"nch\":%u", (unsigned)LED_CHANNELS);

  snprintf(b, sizeof(b), ",\"on\":\"%02u:%02u\",\"off\":\"%02u:%02u\",\"noff\":\"%02u:%02u\",\"nlen\":%u,\"ramp\":%u,\"days\":%u",
           t.onH, t.onM, t.offH, t.offM, t.noffH, t.noffM, (unsigned)t.nightLenMin, (unsigned)t.ramp, (unsigned)t.days);
  w += snprintf(out + w, n - w, "%s", b);

  w += snprintf(out + w, n - w, ",\"time\":\"%02u:%02u:%02u\",\"date\":\"%02u.%02u.%04u\",\"wd\":%u",
                t.hour, t.minute, t.second, t.day, t.month, t.year, t.weekday);
  w += snprintf(out + w, n - w, ",\"tsrc\":\"%s\",\"ip\":\"%s\",\"gw\":\"%s\",\"ssid\":\"%s\",\"host\":\"%s\",\"ap\":%u,\"rssi\":%ld",
                t.timeFromNtp ? "ntp" : (t.timeApprox ? "approx" : "build"),
                t.ip, t.gw, t.ssid, WIFI_HOSTNAME, t.apMode ? 1 : 0, (long)t.rssi);
  w += snprintf(out + w, n - w, ",\"wssid\":\"%s\",\"wst\":\"%s\",\"wcustom\":%u",
                t.wifiSsid, t.wifiStatus, t.wifiCustom ? 1 : 0);
  w += snprintf(out + w, n - w, ",\"ogithub\":%u", (unsigned)(ENABLE_GITHUB_OTA ? 1 : 0));
  w += snprintf(out + w, n - w, ",\"nvm\":%u,\"rstrt\":\"%s\",\"reqs\":%lu,\"cmds\":%lu,\"ota\":%u,\"wd\":%u",
                t.nvmLoaded ? 1 : 0, resetReasonName(resetReason),
                (unsigned long)t.requests, (unsigned long)t.cmds, t.otaActive ? 1 : 0,
                (unsigned)(WATCHDOG_ENABLE ? 1 : 0));
  w += snprintf(out + w, n - w, ",\"al\":%u,\"altext\":\"%s\"", (unsigned)t.alarms, t.alarmText);
  w += snprintf(out + w, n - w, ",\"hist\":%u,\"histmax\":%u,\"step\":%u",
                (unsigned)histCount, (unsigned)HISTORY_MAX_POINTS, (unsigned)HISTORY_PERIOD_S);

  /* журнал циклов нагрева */
  w += snprintf(out + w, n - w, ",\"cyc\":[");
  for (uint8_t k = 0; k < cycleCount; k++) {
    uint8_t idx = (uint8_t)((cycleNext + CYCLE_LOG_SIZE - 1 - k) % CYCLE_LOG_SIZE);
    CycleLog& cl = cycles[idx];
    char ts[8] = "--:--";
    if (cl.startEpoch > 1000000UL) {
      uint8_t h, mi, s, d, mo, wd; uint16_t y;
      epochParts(cl.startEpoch + (uint32_t)TZ_OFFSET_MIN * 60, h, mi, s, d, mo, y, wd);
      snprintf(ts, sizeof(ts), "%02u:%02u", h, mi);
    }
    w += snprintf(out + w, n - w, "%s{\"t\":\"%s\",\"ts\":%d.%02d,\"te\":%d.%02d,\"d\":%u,\"sp\":%d.%02d}",
                  k ? "," : "", ts,
                  cl.tStart100 / 100, abs(cl.tStart100 % 100),
                  cl.tEnd100 / 100, abs(cl.tEnd100 % 100),
                  (unsigned)cl.durSec,
                  cl.set100 / 100, abs(cl.set100 % 100));
  }
  w += snprintf(out + w, n - w, "]}");
  return w;
}

/* ====================== ВЕБ-СЕРВЕР ====================== */
static bool httpReadRequest(WiFiClient& c, char* method, size_t mlen, char* path, size_t plen,
                            char* query, size_t qlen, long* contentLen) {
  char line[320];
  size_t n = 0;
  uint32_t t0 = millis();
  *contentLen = 0;
  method[0] = path[0] = query[0] = 0;

  /* строка запроса */
  while ((millis() - t0) < HTTP_TIMEOUT_MS) {
    if (!c.available()) {
      if (!c.connected()) return false;
      delay(1);
      continue;
    }
    int ch = c.read();
    if (ch < 0) continue;
    if (ch == '\n') break;
    if (ch != '\r' && n < sizeof(line) - 1) line[n++] = (char)ch;
  }
  line[n] = 0;
  if (n == 0) return false;

  char* sp1 = strchr(line, ' ');
  if (!sp1) return false;
  *sp1 = 0;
  strncpy(method, line, mlen - 1);
  char* target = sp1 + 1;
  char* sp2 = strchr(target, ' ');
  if (sp2) *sp2 = 0;
  char* qm = strchr(target, '?');
  if (qm) { *qm = 0; strncpy(query, qm + 1, qlen - 1); }
  strncpy(path, target, plen - 1);

  /* заголовки: интересует Content-Length */
  for (uint8_t i = 0; i < 32; i++) {
    n = 0;
    t0 = millis();
    bool got = false;
    while ((millis() - t0) < HTTP_TIMEOUT_MS) {
      if (!c.available()) {
        if (!c.connected()) break;
        delay(1);
        continue;
      }
      int ch = c.read();
      if (ch < 0) continue;
      if (ch == '\n') { got = true; break; }
      if (ch != '\r' && n < sizeof(line) - 1) line[n++] = (char)ch;
    }
    line[n] = 0;
    if (!got && n == 0) break;
    if (n == 0) break;
    if (!strncasecmp(line, "Content-Length:", 15)) *contentLen = strtol(line + 15, nullptr, 10);
  }
  return true;
}

static void httpSendHead(WiFiClient& c, const char* status, const char* ctype, long len) {
  c.print(F("HTTP/1.1 ")); c.print(status); c.print(F("\r\n"));
  c.print(F("Content-Type: ")); c.print(ctype); c.print(F("\r\n"));
  c.print(F("Cache-Control: no-store\r\nConnection: close\r\n"));
  if (len >= 0) { c.print(F("Content-Length: ")); c.print(len); c.print(F("\r\n")); }
  c.print(F("\r\n"));
}

static void httpSendPage(WiFiClient& c) {
  uint32_t len = (uint32_t)(sizeof(INDEX_HTML) - 1);
  httpSendHead(c, "200 OK", "text/html; charset=utf-8", (long)len);
  uint32_t off = 0;
  while (off < len) {
    uint32_t chunk = len - off;
    if (chunk > 2048) chunk = 2048;
    c.write((const uint8_t*)(INDEX_HTML + off), chunk);
    off += chunk;
    delay(1);
  }
  c.flush();
}

static void httpSendState(WiFiClient& c) {
  static char buf[2200];
  int n = buildStateJson(buf, sizeof(buf));
  httpSendHead(c, "200 OK", "application/json; charset=utf-8", n);
  c.write((const uint8_t*)buf, n);
  c.flush();
}

static void httpSendHistory(WiFiClient& c, uint32_t windowSec) {
  static HistPoint pts[400];
  uint16_t n = historySnapshot(pts, 400, windowSec);
  httpSendHead(c, "200 OK", "application/json; charset=utf-8", -1);
  char b[24];
  c.print(F("{\"n\":"));
  c.print((unsigned)n);
  c.print(F(",\"step\":"));
  c.print((unsigned)(n > 1 ? windowSec / n : HISTORY_PERIOD_S));
  c.print(F(",\"t\":["));
  for (uint16_t i = 0; i < n; i++) { snprintf(b, sizeof(b), "%s%d", i ? "," : "", pts[i].t100); c.print(b); }
  c.print(F("],\"a\":["));
  for (uint16_t i = 0; i < n; i++) { snprintf(b, sizeof(b), "%s%d", i ? "," : "", pts[i].a100); c.print(b); }
  c.print(F("],\"f\":["));
  for (uint16_t i = 0; i < n; i++) { snprintf(b, sizeof(b), "%s%u", i ? "," : "", (unsigned)pts[i].flags); c.print(b); }
  c.print(F("]}"));
  c.flush();
}

static void httpSendScan(WiFiClient& c) {
  httpSendHead(c, "200 OK", "application/json; charset=utf-8", -1);
  DeviceAddress a;
  int n = (int)ds.getDeviceCount();
  c.print(F("{\"ok\":1,\"n\":")); c.print(n);
  c.print(F(",\"water\":")); c.print((unsigned)dsWaterIdx);
  c.print(F(",\"air\":")); c.print((int)dsAirIdx);
  c.print(F(",\"dev\":["));
  char b[8];
  for (int i = 0; i < n; i++) {
    if (!ds.getAddress(a, (uint8_t)i)) continue;
    c.print(i ? "," : ""); c.print('"');
    for (uint8_t k = 0; k < 8; k++) { snprintf(b, sizeof(b), "%02X", a[k]); c.print(b); }
    c.print('"');
  }
  c.print(F("]}"));
  c.flush();
}

/* ---- обновление прошивки прямо из GitHub по кнопке ----
   Плата сама скачивает .bin по адресу OTA_GITHUB_URL (config.h) и
   прошивается им. Ответ идёт без Content-Length, поэтому страница
   получает строки прогресса по мере скачивания:
     S <этап>   I <размер>   P <процент>   D <готово>   E <ошибка>       */
#if ENABLE_GITHUB_OTA
static void httpOtaGithub(WiFiClient& cs, bool dryRun) {
  char b[96];
  cs.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"));
  cs.print(F("Cache-Control: no-store\r\nConnection: close\r\n\r\n"));
  cs.flush();

  if (strlen(OTA_GITHUB_URL) < 12) {
    cs.print(F("E адрес прошивки не задан (OTA_GITHUB_URL в config.h)\n"));
    cs.flush();
    return;
  }

  cs.print(F("S подключаюсь к GitHub\n"));
  cs.flush();
  Serial.println(F("[ota-gh] запрашиваю прошивку с GitHub"));

  static WiFiClientSecure tls;
  tls.setInsecure();                       // без проверки сертификата: экономит 40 КБ и код
  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(tls, OTA_GITHUB_URL)) {
    cs.print(F("E не удалось начать HTTPS-запрос\n"));
    cs.flush();
    return;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(b, sizeof(b), "E GitHub ответил кодом %d\n", code);
    cs.print(b);
    cs.flush();
    http.end();
    Serial.print(F("[ota-gh] ошибка, код ")); Serial.println(code);
    return;
  }
  int len = http.getSize();
  if (len <= 0) {
    cs.print(F("E не удалось узнать размер файла\n"));
    cs.flush();
    http.end();
    return;
  }
  if (len < 200000 || len > 3000000) {     // защита от «не того файла»
    snprintf(b, sizeof(b), "E подозрительный размер %d байт — это не прошивка\n", len);
    cs.print(b);
    cs.flush();
    http.end();
    return;
  }
  snprintf(b, sizeof(b), "I размер прошивки %d КБ\n", len / 1024);
  cs.print(b);
  cs.flush();

  if (dryRun) {
    http.end();
    cs.print(F("D проверка пройдена, файл доступен\n"));
    cs.flush();
    Serial.println(F("[ota-gh] файл доступен, обновление не запускалось"));
    return;
  }

  otaActive = true;
  if (!Update.begin((size_t)len, U_FLASH)) {
    snprintf(b, sizeof(b), "E Update.begin: %s\n", Update.errorString());
    cs.print(b);
    cs.flush();
    otaActive = false;
    http.end();
    return;
  }
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1460];
  int got = 0, lastPct = -1;
  uint32_t t0 = millis();
  bool failed = false;
  while (got < len) {
    size_t avail = stream->available();
    if (avail == 0) {
      if ((millis() - t0) > 30000UL) { failed = true; break; }
      delay(2);
      continue;
    }
    if (avail > sizeof(buf)) avail = sizeof(buf);
    int n = stream->readBytes(buf, avail);
    if (n <= 0) continue;
    if (Update.write(buf, (size_t)n) != (size_t)n) { failed = true; break; }
    got += n;
    t0 = millis();
    int pct = (int)((int64_t)got * 100 / len);
    if (pct != lastPct) {
      lastPct = pct;
      snprintf(b, sizeof(b), "P %d\n", pct);
      cs.print(b);
      cs.flush();
      if (pct % 25 == 0) { Serial.print(F("[ota-gh] ")); Serial.print(pct); Serial.println('%'); }
    }
  }
  http.end();
  if (failed || got != len) {
    snprintf(b, sizeof(b), "E скачивание прервалось: %d из %d байт\n", got, len);
    cs.print(b);
    cs.flush();
    Update.abort();
    otaActive = false;
    Serial.println(F("[ota-gh] ошибка записи, обновление отменено"));
    return;
  }
  if (!Update.end(true)) {
    snprintf(b, sizeof(b), "E Update.end: %s\n", Update.errorString());
    cs.print(b);
    cs.flush();
    otaActive = false;
    return;
  }
  cs.print(F("D прошивка записана, перезагрузка\n"));
  cs.flush();
  Serial.println(F("[ota-gh] прошивка с GitHub записана, перезагружаюсь"));
  delay(800);
  ESP.restart();
}
#else
static void httpOtaGithub(WiFiClient& cs, bool dryRun) {
  (void)dryRun;
  httpSendHead(cs, "200 OK", "text/plain; charset=utf-8", -1);
  cs.print(F("E обновление с GitHub отключено (ENABLE_GITHUB_OTA 0)\n"));
  cs.flush();
}
#endif

/* ---- настройка Wi-Fi со страницы ---- */
static void httpHandleWifi(WiFiClient& c, const char* query) {
  char buf[70];
  if (getParam(query, "forget", buf, sizeof(buf)) && atoi(buf) != 0) {
    wifiForgetReq = true;
    Serial.println(F("[http] запрос: забыть сохранённую сеть"));
    httpSendHead(c, "200 OK", "application/json; charset=utf-8", -1);
    c.print(F("{\"ok\":1,\"forget\":1}"));
    c.flush();
    return;
  }
  char ssid[40] = { 0 }, pass[70] = { 0 };
  if (!getParam(query, "ssid", ssid, sizeof(ssid)) || strlen(ssid) < 1) {
    httpSendHead(c, "400 Bad Request", "application/json; charset=utf-8", -1);
    c.print(F("{\"ok\":0,\"err\":\"не указано имя сети\"}"));
    c.flush();
    return;
  }
  getParam(query, "pass", pass, sizeof(pass));
  snprintf(wifiNewSsid, sizeof(wifiNewSsid), "%s", ssid);
  snprintf(wifiNewPass, sizeof(wifiNewPass), "%s", pass);
  wifiApplyReq = true;
  httpCmds++;
  Serial.print(F("[http] новая сеть Wi-Fi из браузера: \"")); Serial.print(ssid); Serial.println('"');
  httpSendHead(c, "200 OK", "application/json; charset=utf-8", -1);
  c.print(F("{\"ok\":1,\"saved\":1}"));
  c.flush();
}

/* ---- поиск сетей 2.4 ГГц: отдаём простым текстом, чтобы не ломать JSON ---- */
static void httpSendWifiScan(WiFiClient& c) {
  Serial.println(F("[wifi] сканирую сети 2.4 ГГц (2-4 секунды) ..."));
  int n = WiFi.scanNetworks(false, false, false, 400);
  Serial.print(F("[wifi] найдено сетей: ")); Serial.println(n > 0 ? n : 0);
  httpSendHead(c, "200 OK", "text/plain; charset=utf-8", -1);
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    /* убираем управляющие символы и разделитель, чтобы список нельзя было сломать */
    String clean;
    for (size_t k = 0; k < s.length(); k++) {
      char ch = s[k];
      clean += (ch == '|' || (uint8_t)ch < 0x20) ? ' ' : ch;
    }
    c.print((long)WiFi.RSSI(i));
    c.print('|');
    c.print(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1);
    c.print('|');
    c.print(clean);
    c.print('\n');
    yield();
  }
  WiFi.scanDelete();
  c.flush();
}

static void enqueueCmd(const char* key, const char* val) {
  CmdMsg m;
  strncpy(m.key, key, sizeof(m.key) - 1); m.key[sizeof(m.key) - 1] = 0;
  strncpy(m.val, val, sizeof(m.val) - 1); m.val[sizeof(m.val) - 1] = 0;
  if (xQueueSend(cmdQueue, &m, 0) != pdTRUE) Serial.println(F("[cmd] очередь команд переполнена"));}

static void httpHandleCmd(WiFiClient& c, const char* query) {
  /* Разбираем параметры и раскладываем по очереди — применяет контур управления */
  static const char* KEYS[] = { "pump", "air", "fan", "heat", "fanauto", "heatauto",
                                "set", "hyst", "maxt", "fanon", "fanhy", "pw",
                                "lmode", "lch", "lval", "lday", "lnight",
                                "on", "off", "noff", "ramp", "days",
                                "timesync", "conf", "reset", "reboot", "scan", "water", "nightpct" };
  char val[40];
  int applied = 0;
  for (unsigned i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
    if (getParam(query, KEYS[i], val, sizeof(val))) {
      enqueueCmd(KEYS[i], val);
      applied++;
    }
  }
  httpCmds++;
#if LOG_HTTP_CMDS
  Serial.print(F("[http] команда: ")); Serial.print(query);
  Serial.print(F("  (параметров ")); Serial.print(applied); Serial.println(F(")"));
#endif
  httpSendHead(c, "200 OK", "application/json; charset=utf-8", -1);
  c.print(F("{\"ok\":1,\"applied\":")); c.print(applied); c.print('}');
  c.flush();
}

static void httpHandleOta(WiFiClient& c, long contentLen) {
#if ENABLE_WEB_OTA
  if (contentLen <= 0) {
    httpSendHead(c, "400 Bad Request", "application/json; charset=utf-8", -1);
    c.print(F("{\"ok\":0,\"err\":\"нет данных\"}"));
    c.flush();
    return;
  }
  Serial.print(F("[ota] получен образ прошивки, ")); Serial.print(contentLen); Serial.println(F(" байт"));
  otaActive = true;
  if (!Update.begin((size_t)contentLen, U_FLASH)) {
    Serial.print(F("[ota] ошибка начала: ")); Serial.println(Update.errorString());
    otaActive = false;
    httpSendHead(c, "500 Internal Server Error", "application/json; charset=utf-8", -1);
    c.print(F("{\"ok\":0,\"err\":\"Update.begin\"}"));
    c.flush();
    return;
  }
  static uint8_t buf[2048];
  long got = 0;
  uint32_t t0 = millis();
  while (got < contentLen && (millis() - t0) < 120000UL) {
    size_t avail = c.available();
    if (avail == 0) {
      if (!c.connected()) break;
      delay(2);
      continue;
    }
    if (avail > sizeof(buf)) avail = sizeof(buf);
    size_t len = c.read(buf, avail);
    if (len == 0) continue;
    if (Update.write(buf, len) != len) {
      Serial.println(F("[ota] ошибка записи"));
      break;
    }
    got += len;
    t0 = millis();
  }
  bool ok = (got == contentLen) && Update.end(true);
  if (ok) {
    Serial.println(F("[ota] прошивка обновлена, перезагрузка через 1 с"));
    httpSendHead(c, "200 OK", "application/json; charset=utf-8", -1);
    c.print(F("{\"ok\":1,\"reboot\":1}"));
    c.flush();
    delay(1000);
    ESP.restart();
  } else {
    Serial.print(F("[ota] не удалось: ")); Serial.println(Update.errorString());
    otaActive = false;
    Update.abort();
    httpSendHead(c, "500 Internal Server Error", "application/json; charset=utf-8", -1);
    c.print(F("{\"ok\":0,\"err\":\"запись не удалась\"}"));
    c.flush();
  }
#else
  httpSendHead(c, "404 Not Found", "application/json; charset=utf-8", -1);
  c.print(F("{\"ok\":0,\"err\":\"OTA отключено\"}"));
  c.flush();
#endif
}

/* Server-Sent Events: одна долгоживущая связь, интерфейс обновляется мгновенно */
static void sseBegin(WiFiClient& c) {
  c.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"));
  c.print(F("Cache-Control: no-cache\r\nConnection: keep-alive\r\nX-Accel-Buffering: no\r\n\r\n"));
  c.print(F("retry: 3000\n\n"));
  c.flush();
}

static void ssePump(WiFiClient& c) {
  uint32_t now = millis();
  if (now - sseLast < 1000) return;
  sseLast = now;
  int n = buildStateJson(sseJson, sizeof(sseJson));
  c.print(F("data: "));
  c.write((const uint8_t*)sseJson, n);
  c.print(F("\n\n"));
  c.flush();
}

static void httpServe(WiFiClient& c) {
  char method[8], path[96], query[400];
  long clen = 0;
  if (!httpReadRequest(c, method, sizeof(method), path, sizeof(path), query, sizeof(query), &clen)) {
    c.stop(); return;
  }
  httpRequests++;

  if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
    httpSendPage(c);
  } else if (!strcmp(path, "/api/state")) {
    httpSendState(c);
  } else if (!strcmp(path, "/api/history")) {
    uint32_t win = (uint32_t)paramInt(query, "win", 3600);
    if (win < 60) win = 60;
    if (win > (uint32_t)HISTORY_HOURS * 3600UL) win = (uint32_t)HISTORY_HOURS * 3600UL;
    httpSendHistory(c, win);
  } else if (!strcmp(path, "/api/scan")) {
    httpSendScan(c);
  } else if (!strcmp(path, "/api/wifi")) {
    httpHandleWifi(c, query);
  } else if (!strcmp(path, "/api/scanwifi")) {
    httpSendWifiScan(c);
  } else if (!strcmp(path, "/api/updcheck")) {
    httpOtaGithub(c, true);
  } else if (!strcmp(path, "/api/updrun")) {
    httpOtaGithub(c, false);
  } else if (!strcmp(path, "/api/cmd")) {
    httpHandleCmd(c, query);
  } else if (!strcmp(path, "/api/ota")) {
    httpHandleOta(c, clen);
  } else if (!strcmp(path, "/events")) {
    sseClient = c;                     // оставляем связь открытой
    sseBegin(sseClient);
    sseLast = 0;
    return;                            // сокет не закрываем
  } else if (!strcmp(path, "/favicon.ico")) {
    c.print(F("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"));
  } else {
    httpSendHead(c, "404 Not Found", "application/json; charset=utf-8", -1);
    c.print(F("{\"ok\":0,\"err\":\"not found\"}"));
    c.flush();
  }
  delay(2);
  c.stop();
}

static void webTask(void* pv) {
  (void)pv;
  server.begin();
  server.setNoDelay(true);
  Serial.println(F("[http] веб-сервер запущен (порт 80)"));
  for (;;) {
    /* живые данные подписчику */
    if (sseClient && sseClient.connected()) {
      ssePump(sseClient);
    } else if (sseClient) {
      sseClient.stop();
      sseClient = WiFiClient();
    }

#if ENABLE_OTA
    ArduinoOTA.handle();
#endif

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    WiFiClient c = server.accept();        // в 3.x available() переименован в accept()
#else
    WiFiClient c = server.available();
#endif
    if (c) {
      httpServe(c);
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

/* ====================== ИНДИКАЦИЯ ====================== */
static void statusLedTask() {
#if STATUS_RGB_ENABLE
  static uint32_t t = 0;
  static uint8_t phase = 0;
  uint32_t now = millis();
  uint32_t period;
  uint8_t r = 0, g = 0, b = 0;

  if (otaActive)                       { period = 120;  r = 0;   g = 255; b = 255; }
  else if (alarms & (AL_WATER_SENSOR | AL_OVERHEAT | AL_HEAT_TIMEOUT)) { period = 150; r = 255; g = 0; b = 0; }
  else if (tele.apMode)                { period = 900;  r = 160; g = 0;   b = 255; }
  else if (!tele.staConnected)         { period = 400;  r = 0;   g = 0;   b = 255; }
  else                                 { period = 1500; r = 0;   g = 255; b = 40;  }

  if (now - t >= period) {
    t = now;
    phase = !phase;
    rgbWrite(PIN_STATUS_RGB, phase ? r : 0, phase ? g : 0, phase ? b : 0);
  }
#endif
}

/* ====================== ПРИМЕНЕНИЕ КОМАНД ====================== */
static void applyCmd(const char* key, const char* val) {
  bool on;
  auto setRelayFromVal = [&](uint8_t ch) {
    if (!strcmp(val, "toggle")) on = !relayState[ch];
    else on = (!strcmp(val, "1") || !strcmp(val, "on"));
    return on;
  };

  if (!strcmp(key, "pump")) { on = setRelayFromVal(RLY_PUMP); relayWrite(RLY_PUMP, on); cfg.pumpOn = on; cfgDirty = true; }
  else if (!strcmp(key, "air")) { on = setRelayFromVal(RLY_AIR); relayWrite(RLY_AIR, on); cfg.airOn = on; cfgDirty = true; }
  else if (!strcmp(key, "fan")) {
    on = setRelayFromVal(RLY_FAN);
    fanManual = true; fanManualState = on; relayWrite(RLY_FAN, on);
  }
  else if (!strcmp(key, "fanauto")) { fanManual = false; }
  else if (!strcmp(key, "heat")) {
    on = setRelayFromVal(RLY_HEAT);
    if (on && !waterOk) { Serial.println(F("[cmd] нагрев отклонён: нет датчика воды")); return; }
    if (on && waterTemp > cfg.maxWaterTemp) { Serial.println(F("[cmd] нагрев отклонён: перегрев")); return; }
    heatManual = true; heatManualState = on;
    heatManualUntil = millis() + 15UL * 60UL * 1000UL;
    if (!on) { heatManual = false; relayWrite(RLY_HEAT, false); heaterOffAt = millis(); }
  }
  else if (!strcmp(key, "heatauto")) { heatManual = false; }
  else if (!strcmp(key, "set"))   { cfg.setpoint = clampf(atof(val), 15.0f, cfg.maxWaterTemp - 0.5f); cfgDirty = true; }
  else if (!strcmp(key, "hyst"))  { cfg.hyst = clampf(atof(val), 0.1f, 2.0f); cfgDirty = true; }
  else if (!strcmp(key, "maxt"))  { cfg.maxWaterTemp = clampf(atof(val), 20.0f, 40.0f); if (cfg.setpoint > cfg.maxWaterTemp - 0.5f) cfg.setpoint = cfg.maxWaterTemp - 0.5f; cfgDirty = true; }
  else if (!strcmp(key, "fanon")) { cfg.fanOn = clampf(atof(val), 15.0f, 40.0f); cfgDirty = true; }
  else if (!strcmp(key, "fanhy")) { cfg.fanHyst = clampf(atof(val), 0.05f, 2.0f); cfgDirty = true; }
  else if (!strcmp(key, "pw"))    { cfg.heaterPowerW = clampf(atof(val), 5.0f, 1000.0f); cfgDirty = true; }
  else if (!strcmp(key, "lmode")) {
    if (!strcmp(val, "auto")) cfg.lightMode = 0;
    else if (!strcmp(val, "manual")) cfg.lightMode = 1;
    else if (!strcmp(val, "night")) cfg.lightMode = 2;
    cfgDirty = true;
  }
  else if (!strcmp(key, "lch")) { /* яркость задаётся парой lch+lval — см. applyCommands() */ }
  else if (!strcmp(key, "lday")) {
    char* comma = strchr((char*)val, ',');
    if (comma) { *comma = 0; int ch = atoi(val), pct = atoi(comma + 1);
      if (ch >= 0 && ch < LED_CHANNELS) { cfg.dayPct[ch] = (uint8_t)clampf((float)pct, 0, 100); cfgDirty = true; } }
  }
  else if (!strcmp(key, "lnight")) {
    char* comma = strchr((char*)val, ',');
    if (comma) { *comma = 0; int ch = atoi(val), pct = atoi(comma + 1);
      if (ch >= 0 && ch < LED_CHANNELS) { cfg.nightPct[ch] = (uint8_t)clampf((float)pct, 0, 100); cfgDirty = true; } }
  }
  else if (!strcmp(key, "nightpct")) { cfg.nightPct[0] = (uint8_t)clampf(atof(val), 0, 100); cfgDirty = true; }
  else if (!strcmp(key, "on"))    { uint8_t h, m; if (parseHM(val, h, m)) { cfg.onHour = h; cfg.onMin = m; cfgDirty = true; } }
  else if (!strcmp(key, "off"))   { uint8_t h, m; if (parseHM(val, h, m)) { cfg.offHour = h; cfg.offMin = m; cfgDirty = true; } }
  else if (!strcmp(key, "noff"))  { uint8_t h, m; if (parseHM(val, h, m)) { cfg.nightOffHour = h; cfg.nightOffMin = m; cfgDirty = true; } }
  else if (!strcmp(key, "ramp"))  { cfg.rampMin = (uint8_t)clampf(atof(val), 1, 180); cfgDirty = true; }
  else if (!strcmp(key, "days"))  { cfg.daysMask = (uint8_t)(atoi(val) & 0x7F); cfgDirty = true; }
  else if (!strcmp(key, "scan"))  { sensorMap(); printSensorList(); }
  else if (!strcmp(key, "water")) {
    long idx = atol(val);
    DeviceAddress a;
    if (ds.getAddress(a, (uint8_t)idx)) { memcpy(cfg.waterRom, a, 8); cfgDirty = true; sensorMap(); }
  }
  else if (!strcmp(key, "timesync")) { timeApprox = true; timeTask(); }
  else if (!strcmp(key, "conf"))  { settingsSaveNow(); }
  else if (!strcmp(key, "reset")) { settingsDefaults(); settingsSaveNow(); sensorMap(); }
  else if (!strcmp(key, "reboot")) {
    settingsSaveNow();
    Serial.println(F("[sys] перезагрузка по команде с сайта"));
    delay(200);
    ESP.restart();
  }
}

static void applyCommands() {
  CmdMsg m;
  int ch = -1, pct = -1;                 // для пары lch/lval
  while (xQueueReceive(cmdQueue, &m, 0) == pdTRUE) {
    if (!strcmp(m.key, "lch")) { ch = atoi(m.val); continue; }
    if (!strcmp(m.key, "lval")) { pct = atoi(m.val); continue; }
    applyCmd(m.key, m.val);
  }
  if (ch >= 0 && pct >= 0 && ch < LED_CHANNELS) {
    cfg.manualPct[ch] = (uint8_t)clampf((float)pct, 0, 100);
    if (cfg.lightMode == 0) cfg.lightMode = 1;
    cfgDirty = true;
  }
}

/* ====================== РАЗБОР ПАРАМЕТРОВ ====================== */
static bool getParam(const char* q, const char* key, char* out, size_t outLen) {
  if (!q || !*q || outLen < 2) return false;
  size_t klen = strlen(key);
  const char* p = q;
  while (p && *p) {
    if (!strncmp(p, key, klen) && p[klen] == '=') {
      const char* v = p + klen + 1;
      size_t n = 0;
      while (*v && *v != '&' && n < outLen - 1) {
        char chx = *v++;
        if (chx == '%' && v[0] && v[1]) {
          auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
          };
          int hi = hex(v[0]), lo = hex(v[1]);
          if (hi >= 0 && lo >= 0) { chx = (char)(hi * 16 + lo); v += 2; }
        } else if (chx == '+') chx = ' ';
        out[n++] = chx;
      }
      out[n] = 0;
      return true;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  return false;
}

static long paramInt(const char* q, const char* key, long def) {
  char b[24];
  if (!getParam(q, key, b, sizeof(b))) return def;
  return strtol(b, nullptr, 10);
}

static float paramFloat(const char* q, const char* key, float def) {
  char b[24];
  if (!getParam(q, key, b, sizeof(b))) return def;
  return atof(b);
}

/* ====================== ДИАГНОСТИКА ====================== */
static void buildAlarmText() {
  alarmText[0] = 0;
  size_t free_ = sizeof(alarmText) - 1;
  if (alarms & AL_WATER_SENSOR) strncat(alarmText, "Нет датчика воды! ", free_ - strlen(alarmText));
  if (alarms & AL_OVERHEAT)     strncat(alarmText, "Перегрев! ", free_ - strlen(alarmText));
  if (alarms & AL_HEAT_TIMEOUT) strncat(alarmText, "Нагрев выключен по таймеру. ", free_ - strlen(alarmText));
  if (alarms & AL_AIR_SENSOR)   strncat(alarmText, "Нет датчика воздуха. ", free_ - strlen(alarmText));
  if (alarms & AL_NO_TIME)      strncat(alarmText, "Время не синхронизировано. ", free_ - strlen(alarmText));
}

static void logAlarms() {
  if (alarms == lastAlarmBits) return;
  if (alarms & (uint8_t)~lastAlarmBits) {
    Serial.print(F("[тревога] ")); Serial.println(alarmText[0] ? alarmText : "сработала защита");
  } else {
    Serial.println(F("[тревога] снята"));
  }
  lastAlarmBits = alarms;
}

static void printSensorList() {
  uint8_t n = ds.getDeviceCount();
  Serial.print(F("[датчики] найдено устройств 1-Wire: ")); Serial.println((unsigned)n);
  DeviceAddress a;
  for (uint8_t i = 0; i < n; i++) {
    if (!ds.getAddress(a, i)) continue;
    Serial.print(F("  №")); Serial.print((unsigned)i); Serial.print(F("  "));
    for (uint8_t k = 0; k < 8; k++) { if (a[k] < 0x10) Serial.print('0'); Serial.print(a[k], HEX); }
    if (i == dsWaterIdx) Serial.print(F("  → вода (по нему работает термостат)"));
    else if (i == dsAirIdx) Serial.print(F("  → воздух"));
    Serial.println();
  }
  if (n == 0) Serial.println(F("  ! датчиков нет: проверьте резистор 4.7 кОм на 3V3 и провод DQ → GPIO15"));
  else if (dsAirIdx == 0xFF) Serial.println(F("  ! второй датчик (воздух) не найден"));
}

static void printConfigSummary() {
  char b[24];
  Serial.println(F("--- конфигурация ---"));
  Serial.print(F("  прошивка        : ")); Serial.print(F(FW_NAME)); Serial.print(F(" v")); Serial.println(F(FW_VERSION));
  Serial.print(F("  плата           : ESP32-S3, "));
  Serial.print((unsigned)(ESP.getFlashChipSize() / (1024 * 1024))); Serial.print(F(" МБ флеш, "));
  Serial.print((unsigned)(ESP.getPsramSize() / (1024 * 1024))); Serial.print(F(" МБ PSRAM, ядер "));
  Serial.println((unsigned)ESP.getChipCores());
  Serial.print(F("  Wi-Fi сеть      : ")); Serial.println(WIFI_SSID);
  if (strcmp(WIFI_SSID, WIFI_SSID_DEFAULT) == 0)
    Serial.println(F("  ! сеть не изменена в config.h — сразу поднимется точка доступа"));
  Serial.print(F("  реле            : помпа GPIO")); Serial.print(PIN_RELAY_PUMP);
  Serial.print(F(", компрессор GPIO")); Serial.print(PIN_RELAY_AIR);
  Serial.print(F(", вентилятор GPIO")); Serial.print(PIN_RELAY_FAN);
  Serial.print(F(", нагреватель GPIO")); Serial.println(PIN_RELAY_HEAT);
  Serial.print(F("  светильник      : GPIO")); Serial.print(LED_PINS[0]);
  Serial.print(F(", каналов ")); Serial.print((unsigned)LED_CHANNELS);
  Serial.print(F(", ШИМ ")); Serial.print((unsigned)LED_PWM_FREQ / 1000);
  Serial.print(F(" кГц / ")); Serial.print((unsigned)LED_PWM_BITS); Serial.println(F(" бит"));
  snprintf(b, sizeof(b), "   %02u:%02u-%02u:%02u", cfg.onHour, cfg.onMin, cfg.offHour, cfg.offMin);
  Serial.print(F("   расписание      :")); Serial.println(b);
  Serial.print(F("  ночник          : ")); Serial.print((unsigned)cfg.nightPct[0]); Serial.print(F("% до "));
  snprintf(b, sizeof(b), "%02u:%02u", cfg.nightOffHour, cfg.nightOffMin);
  Serial.print(b); Serial.print(F(" (")); Serial.print((unsigned)(nightLenMinutes() / 60));
  Serial.println(F(" ч), дальше темнота"));
  Serial.print(F("  термостат       : "));
  fmtF(b, sizeof(b), cfg.setpoint, 1); Serial.print(b);
  Serial.print(F(" ± ")); fmtF(b, sizeof(b), cfg.hyst, 1); Serial.print(b);
  Serial.print(F(" °C, аварийный порог ")); fmtF(b, sizeof(b), cfg.maxWaterTemp, 1); Serial.print(b);
  Serial.print(F(" °C, ")); fmtF(b, sizeof(b), cfg.heaterPowerW, 0); Serial.print(b); Serial.println(F(" Вт"));
  Serial.print(F("  вентилятор      : выше ")); fmtF(b, sizeof(b), cfg.fanOn, 1); Serial.print(b); Serial.println(F(" °C"));
  Serial.print(F("  настройки       : "));
  Serial.println(settingsFromNvm ? F("загружены из памяти (NVS)") : F("по умолчанию"));
  Serial.print(F("  история         : "));
  Serial.print((unsigned)HISTORY_HOURS); Serial.print(F(" ч в PSRAM, "));
  Serial.print((unsigned)HISTORY_PERIOD_S); Serial.println(F(" с шаг"));
  Serial.println();
}

static void printHeartbeat() {
  char b[24];
  uint32_t up = millis() / 1000UL;
  Serial.println();
  Serial.print(F("[отч] ")); Serial.print((unsigned)tele.hour); Serial.print(':');
  if (tele.minute < 10) Serial.print('0'); Serial.print((unsigned)tele.minute); Serial.print(F("  вода "));
  fmtF(b, sizeof(b), waterOk ? waterTemp : NAN, 2); Serial.print(b);
  Serial.print(F("°C  воздух ")); fmtF(b, sizeof(b), airOk ? airTemp : NAN, 2); Serial.print(b);
  Serial.print(F("°C  уставка ")); fmtF(b, sizeof(b), cfg.setpoint, 1); Serial.print(b); Serial.println(F("°C"));
  Serial.print(F("      реле: помпа ")); Serial.print(relayState[RLY_PUMP] ? F("ВКЛ") : F("выкл"));
  Serial.print(F("  компрессор ")); Serial.print(relayState[RLY_AIR] ? F("ВКЛ") : F("выкл"));
  Serial.print(F("  вентилятор ")); Serial.print(relayState[RLY_FAN] ? F("ВКЛ") : F("выкл"));
  Serial.print(F("  нагрев ")); Serial.print(relayState[RLY_HEAT] ? F("ВКЛ") : F("выкл"));
  Serial.print(F("  свет ")); Serial.print((unsigned)ledPct[0]);
  Serial.print(F("% (")); Serial.print(PHASE_NAMES[(int)lightPhase]); Serial.println(')');
  Serial.print(F("      http://")); Serial.print(ipStr);
  if (tele.apMode) Serial.print(F("  (точка доступа)"));
  Serial.print(F("  RSSI ")); Serial.print((long)tele.rssi);
  Serial.print(F(" dBm  работает ")); Serial.print(up / 3600UL); Serial.print(F(" ч "));
  Serial.print((up % 3600UL) / 60UL); Serial.print(F(" мин  heap "));
  Serial.print(ESP.getFreeHeap() / 1024); Serial.print(F(" КБ  запросов "));
  Serial.println((unsigned long)httpRequests);
  if (alarms) { Serial.print(F("      ТРЕВОГИ: ")); Serial.println(alarmText); }
}

/* ====================== ОТЛАДОЧНЫЙ CLI ====================== */
static void cliHelp() {
  Serial.println(F("--- команды в мониторе порта ---"));
  Serial.println(F("  s — состояние       i — сеть            d — датчики"));
  Serial.println(F("  p — помпа           o — компрессор      v — вентилятор"));
  Serial.println(F("  g — нагрев 15 мин   l — режим света     n — синхронизация времени"));
  Serial.println(F("  x — тест реле 1..3  a — точка доступа   c — сохранить настройки"));
  Serial.println(F("  r — перезагрузка    h — эта справка"));
}

static void cliTask() {
#if SERIAL_CLI_ENABLE
  if (!Serial.available()) return;
  char cmd = (char)Serial.read();
  while (Serial.available()) Serial.read();
  char b[40];
  switch (cmd) {
    case 's':
      printHeartbeat();
      Serial.print(F("      тревоги: ")); Serial.println(alarms ? alarmText : "нет");
      break;
    case 'i':
      Serial.println();
      Serial.print(F("  режим     : ")); Serial.println(tele.apMode ? F("точка доступа + станция") : F("станция"));
      Serial.print(F("  адрес     : http://")); Serial.println(ipStr);
      Serial.print(F("  шлюз      : ")); Serial.println(gwStr);
      Serial.print(F("  RSSI      : ")); Serial.print((long)tele.rssi); Serial.println(F(" dBm"));
      Serial.print(F("  SSID      : ")); Serial.println(WiFi.SSID());
      Serial.print(F("  запросов  : ")); Serial.print((unsigned long)httpRequests);
      Serial.print(F(", команд ")); Serial.println((unsigned long)httpCmds);
      Serial.print(F("  heap/PSRAM: ")); Serial.print(ESP.getFreeHeap() / 1024); Serial.print(F(" КБ / "));
      Serial.print(ESP.getPsramSize() / (1024 * 1024)); Serial.println(F(" МБ"));
      break;
    case 'a': startAP(); break;
    case 'w':
      Serial.println(F("[wifi] переподключаюсь"));
      bannerValid = false;
      WiFi.disconnect();
      connectSta();
      break;
    case 'd': sensorMap(); printSensorList(); break;
    case 'x': {
      Serial.println(F("[тест] по очереди включаю реле 1..3"));
      bool s1 = relayState[RLY_PUMP], s2 = relayState[RLY_AIR], s3 = relayState[RLY_FAN];
      for (uint8_t ch = 0; ch < 3; ch++) {
        Serial.print(F("  канал ")); Serial.println((unsigned)(ch + 1));
        relayWrite(ch, true);  delay(1200); esp_task_wdt_reset();
        relayWrite(ch, false); delay(600);  esp_task_wdt_reset();
      }
      relayWrite(RLY_PUMP, s1); relayWrite(RLY_AIR, s2); relayWrite(RLY_FAN, s3);
      Serial.println(F("[тест] готово"));
      break;
    }
    case 'n': timeApprox = true; timeTask(); Serial.println(F("[time] запросил время у NTP")); break;
    case 'l':
      cfg.lightMode = (uint8_t)((cfg.lightMode + 1) % 3);
      cfgDirty = true;
      Serial.println(cfg.lightMode == 0 ? F("свет: авто") : cfg.lightMode == 2 ? F("свет: ночник") : F("свет: ручной"));
      break;
    case 'p': relayWrite(RLY_PUMP, !relayState[RLY_PUMP]); cfg.pumpOn = relayState[RLY_PUMP]; cfgDirty = true; break;
    case 'o': relayWrite(RLY_AIR, !relayState[RLY_AIR]); cfg.airOn = relayState[RLY_AIR]; cfgDirty = true; break;
    case 'v': fanManual = true; fanManualState = !relayState[RLY_FAN]; relayWrite(RLY_FAN, fanManualState); break;
    case 'g': heatManual = true; heatManualState = true; heatManualUntil = millis() + 15UL * 60UL * 1000UL; break;
    case 'c': settingsSaveNow(); Serial.println(F("[nvs] сохранено")); break;
    case 'r': settingsSaveNow(); Serial.println(F("[sys] перезагрузка")); delay(150); ESP.restart(); break;
    default: cliHelp(); break;
  }
#endif
}

/* ====================== SETUP / LOOP ====================== */
void setup() {
  /* 1. Сначала — безопасное состояние реле */
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
#if RELAY_ACTIVE_LOW
    digitalWrite(RELAY_PINS[i], HIGH);          // ВЫКЛЮЧЕНО
#else
    digitalWrite(RELAY_PINS[i], LOW);
#endif
  }
  relayState[RLY_PUMP] = relayState[RLY_AIR] = relayState[RLY_FAN] = relayState[RLY_HEAT] = 0;

  /* 2. Порт */
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) { }
  delay(50);
  resetReason = esp_reset_reason();

  Serial.println();
  Serial.println(F("============================================================"));
  Serial.print(F("   ")); Serial.print(F(FW_NAME)); Serial.print(F("  v")); Serial.print(F(FW_VERSION));
  Serial.println(F("   ·   ESP32-S3 (N16R8)"));
  Serial.print(F("   причина перезагрузки: ")); Serial.println(resetReasonName(resetReason));
  Serial.print(F("   ядер ")); Serial.print((unsigned)ESP.getChipCores());
  Serial.print(F(", частота ")); Serial.print((unsigned)ESP.getCpuFreqMHz());
  Serial.print(F(" МГц, флеш ")); Serial.print((unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  Serial.print(F(" МБ, PSRAM ")); Serial.print((unsigned)(ESP.getPsramSize() / (1024 * 1024)));
  Serial.println(F(" МБ"));
  Serial.println(F("============================================================"));

  /* 3. Общие объекты */
  stateMux = xSemaphoreCreateMutex();
  cmdQueue = xQueueCreate(24, sizeof(CmdMsg));

  /* 4. Настройки */
  bool loaded = settingsLoad();
  Serial.println(loaded ? F("[nvs] настройки загружены из энергонезависимой памяти")
                        : F("[nvs] настройки по умолчанию (первый запуск)"));

  /* 5. Матрица гаммы и ШИМ */
  for (uint8_t i = 0; i <= 100; i++) {
    float v = powf((float)i / 100.0f, LED_PWM_GAMMA) * (float)((1 << LED_PWM_BITS) - 1) + 0.5f;
    gammaLut[i] = (uint16_t)clampf(v, 0.0f, (float)((1 << LED_PWM_BITS) - 1));
  }
  for (uint8_t i = 0; i < LED_CHANNELS; i++) {
    pwmInit(i, LED_PINS[i], LED_PWM_FREQ, LED_PWM_BITS);
    pwmWrite(i, LED_PINS[i], 0);
  }
#if FAN_PWM_ENABLE
  pwmInit(3, PIN_FAN_PWM, 25000, LED_PWM_BITS);
  pwmWrite(3, PIN_FAN_PWM, 0);
#endif

  /* 6. История в PSRAM */
  historyInit();

  /* 7. Датчики */
  ds.begin();
  ds.setWaitForConversion(false);
  ds.setResolution(12);
  sensorMap();
  printSensorList();

  /* 8. Время */
  timeBegin();

  /* 9. Реле в сохранённые состояния */
  relayWrite(RLY_PUMP, cfg.pumpOn != 0);
  relayWrite(RLY_AIR, cfg.airOn != 0);
  heaterOffAt = millis() - MIN_HEATER_OFF_MS;

  Serial.println();
  printConfigSummary();

  /* 10. Сеть и веб-задача на втором ядре */
  netBegin();

#if ENABLE_OTA
  ArduinoOTA.setHostname(WIFI_HOSTNAME);
  ArduinoOTA.onStart([]() {
    otaActive = true;
    Serial.println(F("[ota] начало обновления по воздуху"));
  });
  ArduinoOTA.onEnd([]() { Serial.println(F("[ota] обновление завершено")); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int total) {
    static uint8_t lastPct = 255;
    uint8_t pct = total ? (uint8_t)(p * 100 / total) : 0;
    if (pct != lastPct && pct % 10 == 0) { lastPct = pct; Serial.print(F("[ota] ")); Serial.print(pct); Serial.println(F("%")); }
  });
  ArduinoOTA.onError([](ota_error_t e) {
    otaActive = false;
    Serial.print(F("[ota] ошибка ")); Serial.println((int)e);
  });
  ArduinoOTA.begin();
#endif

  xTaskCreatePinnedToCore(webTask, "web", 10240, nullptr, 2, &webTaskHandle, 0);

  /* 11. Сторожевой таймер */
#if WATCHDOG_ENABLE
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  /* IDF 5: таймер уже инициализирован ядром — меняем период и подписываем loop-задачу */
  {
    esp_task_wdt_config_t twdtCfg = {};
    twdtCfg.timeout_ms = (uint32_t)WATCHDOG_TIMEOUT_S * 1000UL;
    twdtCfg.idle_core_mask = 0;
    twdtCfg.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&twdtCfg) != ESP_OK) esp_task_wdt_init(&twdtCfg);
  }
#else
  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(nullptr);
  Serial.print(F("[wdt] сторожевой таймер включён, "));
  Serial.print(WATCHDOG_TIMEOUT_S);
  Serial.println(F(" с"));
#endif

  lastHeatTick = millis();
  Serial.println();
  Serial.println(F("[подсказка] h — список команд, s — состояние, a — точка доступа"));
}

void loop() {
#if WATCHDOG_ENABLE
  esp_task_wdt_reset();
#endif

  /* монитор порта открыли позже — печатаем сводку заново */
  bool serialOpen = (bool)Serial;
  if (serialOpen && !serialWasOpen) {
    serialWasOpen = true;
    Serial.println();
    Serial.println(F("[монитор] порт открыт — печатаю текущее состояние"));
    printConfigSummary();
    if (bannerValid) printNetBanner(tele.apMode ? "режим точки доступа" : "подключено к роутеру");
    else Serial.println(F("  адрес пока не получен — плата подключается к сети...\n"));
    printSensorList();
    cliHelp();
  } else if (!serialOpen) {
    serialWasOpen = false;
  }

  applyCommands();                // команды с сайта
  netTask();
  sensorTask();
  controlHeater();
  controlFan();
  controlLight();
  timeTask();
  settingsTask();
  buildAlarmText();
  logAlarms();
  statusLedTask();
  cliTask();
  publishTelemetry();

  uint32_t now = millis();
  if (histCount == 0 || (now - histAt) >= (HISTORY_PERIOD_S * 1000UL)) {
    histAt = now;
    historyPush();
  }

  /* наработка нагревателя */
  uint32_t dt = now - lastHeatTick;
  lastHeatTick = now;
  if (relayState[RLY_HEAT]) { heatMsTotal += dt; heatMsToday += dt; }

  /* суточный сброс счётчика энергии */
  if (tele.day != lastDay) { lastDay = tele.day; heatMsToday = 0; }

  /* предупреждение о времени */
  if (timeApprox && millis() > 120000UL) alarms |= AL_NO_TIME;
  else if (!timeApprox) alarms &= (uint8_t)~AL_NO_TIME;

  /* сводка в монитор порта */
  if (now - lastHeartbeat >= LOG_HEARTBEAT_MS) {
    lastHeartbeat = now;
    printHeartbeat();
  }

  delay(10);
}
