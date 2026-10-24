/* =====================================================================
   Аквариумный контроллер на Arduino MKR WiFi 1010
   =====================================================================
   Возможности:
     1. Светильник: рассвет — день — закат, ШИМ-диммирование (1..3 канала)
     2. Реле 1 — помпа          (кнопка на сайте, состояние переживает сброс)
     3. Реле 2 — компрессор     (кнопка на сайте, состояние переживает сброс)
     4. Реле 3 — вентилятор     (авто по температуре воды + ручной режим)
     5. Реле 4 — подогреватель 75 Вт (термостат с гистерезисом и защитами)
     + два DS18B20 (вода и воздух), часы NTP, веб-сервер с сайтом,
       график температур, журнал циклов нагрева, счётчик энергии.

   Настройки — в файле config.h, веб-страница — в webui.h,
   хранение настроек во флеш-памяти — nvm.h.

   Требуемые библиотеки (Менеджер библиотек Arduino IDE):
     OneWire (Paul Stoffregen), DallasTemperature (Miles Burton), RTCZero (Arduino)
   WiFiNINA ставится вместе с платой "Arduino MKR WiFi 1010".
   ===================================================================== */

#include <WiFiNINA.h>
#include <WiFiUdp.h>
#include <RTCZero.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

#include "config.h"
#include "nvm.h"
#include "webui.h"

/* ====================== СЛУЖЕБНЫЕ КОНСТАНТЫ ====================== */
#define SETTINGS_MAGIC    0xA9C1
#define SETTINGS_VERSION  4

/* номера реле */
#define RLY_PUMP  0
#define RLY_AIR   1
#define RLY_FAN   2
#define RLY_HEAT  3
static const uint8_t RELAY_PINS[4] = {
  PIN_RELAY_PUMP, PIN_RELAY_AIR, PIN_RELAY_FAN, PIN_RELAY_HEAT
};

/* биты тревог */
#define AL_WATER_SENSOR  0x01   // нет данных о температуре воды  (критично)
#define AL_OVERHEAT      0x04   // перегрев                      (критично)
#define AL_HEAT_TIMEOUT  0x08   // нагрев дольше предела         (критично)
#define AL_AIR_SENSOR    0x02   // нет датчика воздуха          (предупреждение)
#define AL_NO_TIME       0x10   // время не синхронизировано    (предупреждение)

/* фазы светильника */
enum LightPhase { LP_NIGHT = 0, LP_DAWN, LP_DAY, LP_DUSK, LP_MANUAL };
static const char* const PHASE_NAMES[] = { "night", "dawn", "day", "dusk", "manual" };

/* ====================== СТРУКТУРЫ ====================== */
struct Settings {
  uint16_t magic;
  uint8_t  ver;
  uint8_t  crc;
  uint8_t  ledCh;                 // число каналов света при сохранении
  uint8_t  pumpOn;                // состояние помпы после включения питания
  uint8_t  airOn;                 // состояние компрессора
  uint8_t  lightMode;             // 0 = авто (расписание), 1 = ручной
  uint8_t  onHour, onMin;         // начало рассвета
  uint8_t  offHour, offMin;       // конец заката
  uint8_t  nightOffHour, nightOffMin; // когда ночник гаснет совсем
  uint8_t  rampMin;               // длительность рассвета/заката, мин
  uint8_t  daysMask;              // бит0 = Вс ... бит6 = Сб
  uint8_t  dayPct[3];             // яркость днём, %
  uint8_t  nightPct[3];           // яркость ночью (лунный свет), %
  uint8_t  manualPct[3];          // яркость в ручном режиме, %
  uint8_t  waterRom[8];           // ROM-адрес датчика воды (нули = по порядку)
  float    setpoint;              // уставка температуры воды, °C
  float    hyst;                  // полуширина полосы термостата, °C
  float    maxWaterTemp;          // аварийный порог перегрева, °C
  float    fanOn;                 // порог включения вентилятора, °C
  float    fanHyst;               // гистерезис вентилятора, °C
  float    heaterPowerW;          // мощность подогревателя, Вт (для статистики)
};

struct CycleLog {                 // один цикл нагрева
  uint32_t startEpoch;            // время включения (0 = время неизвестно)
  uint16_t durSec;                // длительность нагрева, с
  int16_t  tStart100;             // температура на старте, °C*100
  int16_t  tEnd100;               // температура в конце, °C*100
  int16_t  set100;                // уставка на момент цикла, °C*100
};

struct HistPoint {
  int16_t t100;                   // температура воды, °C*100
  int16_t a100;                   // температура воздуха, °C*100
  uint8_t flags;                  // бит0 нагрев, 1 вентилятор, 2 помпа, 3 аэратор, 4 свет
};

struct DateTime {
  uint16_t year; uint8_t month, day, hour, minute, second, weekday;
};

/* ====================== НАСТРОЙКИ И СОСТОЯНИЕ ====================== */
static Settings cfg;

/* датчики */
static OneWire oneWire(PIN_ONEWIRE);
static DallasTemperature ds(&oneWire);
static float waterTemp = NAN;      // температура воды, °C
static float airTemp   = NAN;      // температура воздуха, °C
static float waterShown = NAN;     // сглаженное значение для показа
static bool  waterOk = false, airOk = false;
static uint8_t waterBad = 0, airBad = 0;
static uint8_t dsWaterIdx = 0, dsAirIdx = 0xFF;

/* время */
static RTCZero rtc;
static bool  timeFromNtp = false;
static uint32_t ntpTryAt = 0, ntpOkAt = 0;
static uint8_t  ntpFail = 0;
static uint8_t  lastDay = 255;

/* реле */
static bool relayState[4] = { false, false, false, false };
static uint32_t heaterOnAt = 0, heaterOffAt = 0;
static bool heaterLocked = false;
static uint32_t heaterLockUntil = 0;
static bool heatManual = false, heatManualState = false;
static uint32_t heatManualUntil = 0;
static bool fanManual = false, fanManualState = false;
static uint8_t alarms = 0;

/* статистика нагрева */
static uint32_t heatMsTotal = 0, heatMsToday = 0;
static uint32_t lastHeatTick = 0;

/* журнал циклов */
static CycleLog cycles[CYCLE_LOG_SIZE];
static uint8_t cycleCount = 0, cycleNext = 0;
static bool  cycleActive = false;
static uint32_t cycleStartMs = 0;
static float cycleStartT = 0;

/* свет */
static LightPhase lightPhase = LP_NIGHT;
static uint8_t ledPct[LED_CHANNELS] = { 0 };
static uint8_t ledOut[LED_CHANNELS] = { 0 };
static uint8_t gammaLut[101];

/* история для графика */
static HistPoint hist[HISTORY_POINTS];
static uint16_t histCount = 0, histNext = 0, histNum = 0;
static uint32_t histAt = 0;

/* флаг «настройки изменились» — запись во флеш откладывается */
static bool cfgDirtyFlag = false;
static bool settingsFromNvm = false;

/* сеть */
static WiFiServer server(HTTP_PORT);
static WiFiUDP    ntpUdp;
static char ipStr[16] = "0.0.0.0";
static char gwStr[16] = "0.0.0.0";
static bool apMode = false;
static bool wifiWasConnected = false;
static uint32_t wifiNextTry = 0, apNextTry = 0;
static int32_t rssi = 0;
static uint8_t lastWlStatus = 255;      // для логов: предыдущий статус Wi-Fi
static uint32_t staLostSince = 0;       // с какого времени потеряна связь (0 = связи не теряли)
static bool wasInAp = false;            // вышли из точки доступа и пробуем роутер
static uint32_t apRetryUntil = 0;       // до какого времени ждём роутер, потом вернём точку доступа
static bool bannerShown = false;        // баннер с адресом уже напечатан
static bool bannerValid = false;        // адрес в баннере актуален

/* диагностика */
static uint32_t httpRequests = 0;       // всего запросов к веб-серверу
static uint32_t httpCmds = 0;           // из них команд
static uint8_t  resetCauseBits = 0;     // причина последней перезагрузки
static uint32_t lastHeartbeat = 0;
static uint8_t  lastAlarmBits = 0;
static bool     serialWasOpen = false;  // монитор порта был открыт

/* служебное */
static char alarmText[64] = "";

/* ====================== ПРОТОТИПЫ ====================== */
static float    clampf(float v, float lo, float hi);
static void     fmtF(char* out, size_t n, float v, uint8_t dec);
static uint8_t  crc8(const uint8_t* p, size_t n);
static uint8_t  settingsCrc(const Settings& s);
static int      hexVal(char c);
static bool     parseHM(const char* s, uint8_t& h, uint8_t& m);
static void     settingsDefaults();
static bool     settingsLoad();
static void     settingsSaveNow();
static void     settingsMarkDirty();
static void     settingsTask();

static void     timeBegin();
static bool     ntpSync();
static void     timeTask();
static uint32_t nowEpoch();
static void     epochToLocal(uint32_t epoch, DateTime& dt);

static void     sensorMap();
static void     sensorTask();

static void     relayWrite(uint8_t ch, bool on);
static void     heaterTask();
static void     fanTask();
static void     lightTask();
static void     ledWrite(uint8_t ch, uint8_t pct);
static uint32_t estimateHeaterSeconds(float dtC);
static uint16_t nightLenMinutes();

static void     historyPush();

static void     wifiTask();
static void     connectSta();
static void     startAP();
static const char* wlStatusName(uint8_t st);
static void     printNetBanner(const char* why);
static void     printConfigSummary();
static void     printSensorList();
static void     printHeartbeat();
static void     logAlarms();
static void     wdtBegin();
static void     wdtFeed();
static uint8_t  readResetCause();
static const char* resetCauseName(uint8_t bits);
static void     httpTask();
static void     handleRequest(WiFiClient& c, const char* method, char* path, const char* query, char* body);
static bool     readLine(WiFiClient& c, char* buf, size_t maxLen);
static void     streamHead(WiFiClient& c, const char* status, const char* ctype);
static void     sendPage(WiFiClient& c);
static void     sendState(WiFiClient& c);
static void     sendHistory(WiFiClient& c);
static void     sendScan(WiFiClient& c);
static bool     httpParam(const char* q, const char* key, char* out, size_t outLen);
static long     httpInt(const char* q, const char* key, long def);
static float    httpFloat(const char* q, const char* key, float def);
static void     handleCmd(const char* q, char* reply, size_t replyLen);

static void     buildAlarmText();
static void     statusLedTask();
static void     cliTask();

/* ====================== МЕЛКИЕ УТИЛИТЫ ====================== */
static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

/* Форматирование float без %f: на SAMD21 printf с плавающей точкой
   не работает (newlib-nano без _printf_float), поэтому считаем сами. */
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

static uint8_t crc8(const uint8_t* p, size_t n) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

static uint8_t settingsCrc(const Settings& s) {
  Settings t = s;
  t.crc = 0;
  return crc8((const uint8_t*)&t, sizeof(Settings));
}

/* "08:30" -> часы и минуты (без sscanf, чтобы не тянуть лишнее из libc) */
static bool parseHM(const char* s, uint8_t& h, uint8_t& m) {
  if (!s) return false;
  while (*s == ' ') s++;
  if (*s < '0' || *s > '9') return false;
  int hh = 0, mm = 0;
  if (s[1] >= '0' && s[1] <= '9') { hh = (s[0] - '0') * 10 + (s[1] - '0'); s += 2; }
  else { hh = s[0] - '0'; s += 1; }
  if (*s == ':' || *s == '.') {
    s++;
    if (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9') mm = (s[0] - '0') * 10 + (s[1] - '0');
    else if (s[0] >= '0' && s[0] <= '9') mm = s[0] - '0';
  }
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
  h = (uint8_t)hh;
  m = (uint8_t)mm;
  return true;
}

/* ====================== НАСТРОЙКИ ====================== */
static void settingsDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = SETTINGS_MAGIC;
  cfg.ver = SETTINGS_VERSION;
  cfg.ledCh = LED_CHANNELS;
  cfg.pumpOn = 1;                    // помпа после сброса — включена
  cfg.airOn = 1;                     // аэратор после сброса — включён
  cfg.lightMode = 0;
  cfg.onHour = DEF_LIGHT_ON_H;  cfg.onMin = DEF_LIGHT_ON_M;
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
  cfg.crc = settingsCrc(cfg);
}

static bool settingsLoad() {
#if NVM_ENABLE
  Settings tmp;
  nvm::read(&tmp, sizeof(Settings));
  if (tmp.magic == SETTINGS_MAGIC && tmp.ver == SETTINGS_VERSION &&
      tmp.ledCh == LED_CHANNELS && tmp.crc == settingsCrc(tmp)) {
    cfg = tmp;
    settingsFromNvm = true;
    return true;
  }
#endif
  settingsDefaults();
  return false;
}

static void settingsSaveNow() {
#if NVM_ENABLE
  cfg.magic = SETTINGS_MAGIC;
  cfg.ver = SETTINGS_VERSION;
  cfg.ledCh = LED_CHANNELS;
  cfg.crc = settingsCrc(cfg);
  nvm::write(&cfg, sizeof(Settings));
#endif
}

static void settingsMarkDirty() {
  cfgDirtyFlag = true;
}

/* Запись во флеш откладывается: флеш имеет ограниченный ресурс перезаписи,
   а пользователь может крутить слайдеры десятки раз подряд. */
static void settingsTask() {
  static bool pending = false;
  static uint32_t pendingSince = 0;
  uint32_t now = millis();

  if (cfgDirtyFlag && !pending) {
    pending = true;
    pendingSince = now;
  }
  if (pending && (now - pendingSince) >= NVM_SAVE_DEBOUNCE_MS) {
    pending = false;
    cfgDirtyFlag = false;
    settingsSaveNow();
    settingsFromNvm = true;
    Serial.println(F("[nvm] настройки сохранены"));
  }
}

/* ====================== ВРЕМЯ ====================== */
static void epochToLocal(uint32_t epoch, DateTime& dt) {
  int32_t t = (int32_t)epoch + (int32_t)TZ_OFFSET_MIN * 60;
  int32_t days = t / 86400;
  int32_t rem  = t % 86400;
  if (rem < 0) { rem += 86400; days--; }
  dt.hour = (uint8_t)(rem / 3600);
  dt.minute = (uint8_t)((rem % 3600) / 60);
  dt.second = (uint8_t)(rem % 60);
  int32_t wd = (days + 4) % 7;                 // 01.01.1970 — четверг
  if (wd < 0) wd += 7;
  dt.weekday = (uint8_t)wd;                    // 0 = воскресенье

  int32_t z = days + 719468;
  int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t y = (int32_t)yoe + era * 400;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int32_t mp = (int32_t)((5 * doy + 2) / 153);
  uint32_t d = doy - (uint32_t)((153 * mp + 2) / 5) + 1;
  int32_t m = mp + (mp < 10 ? 3 : -9);
  dt.year = (uint16_t)(y + (m <= 2 ? 1 : 0));
  dt.month = (uint8_t)m;
  dt.day = (uint8_t)d;
}

static uint32_t nowEpoch() { return rtc.getEpoch(); }

/* время компиляции — грубый запасной вариант, если NTP недоступен */
static void rtcBeginFromBuild() {
  static const char* mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* d = __DATE__;                    // "Mmm dd yyyy"
  const char* t = __TIME__;                    // "hh:mm:ss"
  int month = 1;
  for (int i = 0; i < 12; i++) {
    if (strncmp(d, mon + i * 3, 3) == 0) { month = i + 1; break; }
  }
  int day = atoi(d + 4);
  int year = atoi(d + 7);
  int hour = atoi(t);
  int minute = atoi(t + 3);
  int second = atoi(t + 6);
  /* перевод даты в unix-время (алгоритм days_from_civil) */
  int yy = year - (month <= 2 ? 1 : 0);
  int32_t era = (yy >= 0 ? yy : yy - 399) / 400;
  uint32_t yoe = (uint32_t)(yy - era * 400);
  uint32_t doy = (uint32_t)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
  uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int32_t days = (int32_t)era * 146097 + (int32_t)doe - 719468;
  uint32_t epoch = (uint32_t)((int32_t)days * 86400 + hour * 3600 + minute * 60 + second);
  epoch -= (uint32_t)((int32_t)TZ_OFFSET_MIN * 60);   // в RTC храним UTC
  rtc.setEpoch(epoch);
  Serial.println(F("[time] RTC выставлены по времени компиляции (нет NTP)"));
}

static bool ntpSync() {
  uint8_t buf[48];
  memset(buf, 0, sizeof(buf));
  buf[0] = 0x23;                               // LI=0, VN=4, Mode=3 (client)
  ntpUdp.beginPacket(NTP_SERVER, 123);
  ntpUdp.write(buf, sizeof(buf));
  if (ntpUdp.endPacket() == 0) return false;

  uint32_t t0 = millis();
  while ((millis() - t0) < NTP_TIMEOUT_MS) {
    wdtFeed();                                  // блокирующее ожидание — кормим таймер
    int sz = ntpUdp.parsePacket();
    if (sz >= 48) {
      ntpUdp.read(buf, sizeof(buf));
      uint32_t secs = ((uint32_t)buf[40] << 24) | ((uint32_t)buf[41] << 16) |
                      ((uint32_t)buf[42] << 8)  | (uint32_t)buf[43];
      if (secs > 1700000000UL) {                // правдоподобная дата (2023+)
        rtc.setEpoch(secs - 2208988800UL);      // NTP -> unix
        timeFromNtp = true;
        ntpOkAt = millis();
        ntpFail = 0;
        DateTime dt; epochToLocal(nowEpoch(), dt);
        Serial.print(F("[time] NTP OK, местное время "));
        Serial.print(dt.day); Serial.print('.'); Serial.print(dt.month); Serial.print('.');
        Serial.print(dt.year); Serial.print(' ');
        Serial.print(dt.hour); Serial.print(':');
        if (dt.minute < 10) Serial.print('0');
        Serial.println(dt.minute);
        return true;
      }
    }
    delay(10);
  }
  return false;
}

static void timeBegin() {
  rtc.begin();
  rtcBeginFromBuild();
}

static void timeTask() {
  uint32_t now = millis();
  bool needSync = (!timeFromNtp) ||
                  (ntpOkAt != 0 && (now - ntpOkAt) > NTP_SYNC_PERIOD_MS);
  if (needSync && (now - ntpTryAt) > (ntpFail ? NTP_RETRY_MS * 6 : 5000UL)) {
    ntpTryAt = now;
    if (ntpSync()) ntpFail = 0; else if (ntpFail < 250) ntpFail++;
  }
  /* сброс суточного счётчика энергии в полночь */
  DateTime dt; epochToLocal(nowEpoch(), dt);
  if (dt.day != lastDay) {
    lastDay = dt.day;
    heatMsToday = 0;
  }
}

/* ====================== ДАТЧИКИ ====================== */
static void sensorMap() {
  uint8_t n = ds.getDeviceCount();
  dsWaterIdx = 0;
  dsAirIdx = 0xFF;
  if (n == 0) return;

  bool romSet = false;
  for (uint8_t i = 0; i < 8; i++) if (cfg.waterRom[i]) { romSet = true; break; }
  if (romSet) {
    DeviceAddress a;
    for (uint8_t i = 0; i < n; i++) {
      if (ds.getAddress(a, i) && memcmp(a, cfg.waterRom, 8) == 0) { dsWaterIdx = i; break; }
    }
  }
  for (uint8_t i = 0; i < n; i++) {
    if (i != dsWaterIdx) { dsAirIdx = i; break; }
  }
  Serial.print(F("[sensor] найдено датчиков: ")); Serial.println(n);
}

static void sensorTask() {
  static uint8_t state = 0;
  static uint32_t t0 = 0, lastReq = 0;
  uint32_t now = millis();

  if (state == 0) {
    if (now - lastReq >= SENSOR_PERIOD_MS) {
      ds.requestTemperatures();               // без ожидания (setWaitForConversion(false))
      state = 1;
      t0 = now;
    }
    return;
  }
  if (now - t0 < SENSOR_CONV_MS) return;
  state = 0;
  lastReq = now;

  /* --- вода --- */
  float tw = ds.getTempCByIndex(dsWaterIdx);
  bool wOk = (tw > TEMP_MIN_VALID && tw < TEMP_MAX_VALID && tw != 85.0f);
  if (wOk) {
    waterTemp = tw;
    waterBad = 0;
    waterOk = true;
    waterShown = isnan(waterShown) ? tw : (waterShown + 0.35f * (tw - waterShown));
    alarms &= (uint8_t)~AL_WATER_SENSOR;
  } else if (waterBad < 250) {
    waterBad++;
    if (waterBad >= SENSOR_FAIL_COUNT) {
      waterOk = false;
      alarms |= AL_WATER_SENSOR;
    }
  }

  /* --- воздух --- */
  if (dsAirIdx != 0xFF) {
    float ta = ds.getTempCByIndex(dsAirIdx);
    bool aOk = (ta > TEMP_MIN_VALID && ta < TEMP_MAX_VALID && ta != 85.0f);
    if (aOk) {
      airTemp = ta; airBad = 0; airOk = true;
      alarms &= (uint8_t)~AL_AIR_SENSOR;
    } else if (airBad < 250) {
      airBad++;
      if (airBad >= SENSOR_FAIL_COUNT) { airOk = false; alarms |= AL_AIR_SENSOR; }
    }
  } else {
    airOk = false;
    alarms |= AL_AIR_SENSOR;
  }
}

/* ====================== ИСПОЛНИТЕЛЬНЫЕ УСТРОЙСТВА ====================== */
static const char* const RELAY_NAMES[4] = { "помпа", "компрессор", "вентилятор", "нагреватель" };

static void relayWrite(uint8_t ch, bool on) {
  if (ch > 3) return;
  if (relayState[ch] == on) return;
#if RELAY_ACTIVE_LOW
  digitalWrite(RELAY_PINS[ch], on ? LOW : HIGH);
#else
  digitalWrite(RELAY_PINS[ch], on ? HIGH : LOW);
#endif
  relayState[ch] = on;

  /* каждое переключение попадает в лог — по нему видно, что плата живая */
  char b[16];
  Serial.print(F("[реле] канал "));
  Serial.print((unsigned)(ch + 1));
  Serial.print(' ');
  Serial.print(RELAY_NAMES[ch]);
  Serial.print(on ? F(" ВКЛ") : F(" выкл"));
  if (ch == RLY_HEAT && waterOk) {
    Serial.print(F("  (вода "));
    fmtF(b, sizeof(b), waterTemp, 2);
    Serial.print(b);
    Serial.print(F(" °C, уставка "));
    fmtF(b, sizeof(b), cfg.setpoint, 1);
    Serial.print(b);
    Serial.print(F(" °C)"));
  } else if (ch == RLY_FAN && waterOk) {
    Serial.print(F("  (вода "));
    fmtF(b, sizeof(b), waterTemp, 2);
    Serial.print(b);
    Serial.print(F(" °C)"));
  }
  Serial.println();
}

static uint32_t estimateHeaterSeconds(float dtC) {
  /* сколько секунд нужно, чтобы нагреть аквариум на dtC градусов.
     Оценка по мощности нагревателя и объёму (объём берём из настроек нет,
     поэтому считаем по «тепловой постоянной» 200000 Дж/°C — примерно 50 л).
     Значение используется только для подсказки в интерфейсе. */
  float J = 200000.0f * dtC;
  float P = cfg.heaterPowerW > 1.0f ? cfg.heaterPowerW : 75.0f;
  return (uint32_t)(J / P);
}

static void heaterTask() {
  uint32_t now = millis();
  bool want = relayState[RLY_HEAT];
  bool decided = false;

  /* ручной режим: включаем по кнопке, но с автоматическим отключением */
  if (heatManual) {
    if ((int32_t)(now - heatManualUntil) >= 0) {
      heatManual = false;                     // время вышло — вернулись в авто
      heatManualState = false;
    } else {
      want = heatManualState;
      if (waterOk && waterTemp > cfg.maxWaterTemp) {   // защита работает и в ручном режиме
        want = false;
        alarms |= AL_OVERHEAT;
      }
      decided = true;
    }
  }

  if (!decided) {
    if (!waterOk) {
      want = false;                                  // нет данных — греть нельзя
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

        if (want && !relayState[RLY_HEAT] && (now - heaterOffAt) < MIN_HEATER_OFF_MS)
          want = false;                              // пауза между включениями
        if (!want && relayState[RLY_HEAT] && (now - heaterOnAt) < MIN_HEATER_ON_MS)
          want = true;                               // не дёргаем реле слишком часто

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
      heaterOnAt = now;
      cycleActive = true;
      cycleStartMs = now;
      cycleStartT = waterOk ? waterTemp : NAN;
    } else {
      heaterOffAt = now;
      if (cycleActive) {
        cycleActive = false;
        CycleLog& cl = cycles[cycleNext];
        cl.startEpoch = nowEpoch() - (now - cycleStartMs) / 1000UL;
        cl.durSec = (uint16_t)((now - cycleStartMs) / 1000UL);
        cl.tStart100 = (int16_t)lroundf(cycleStartT * 100.0f);
        cl.tEnd100 = waterOk ? (int16_t)lroundf(waterTemp * 100.0f) : 0;
        cl.set100 = (int16_t)lroundf(cfg.setpoint * 100.0f);
        cycleNext = (uint8_t)((cycleNext + 1) % CYCLE_LOG_SIZE);
        if (cycleCount < CYCLE_LOG_SIZE) cycleCount++;
      }
    }
  }
}

static void fanTask() {
  if (fanManual) { relayWrite(RLY_FAN, fanManualState); return; }
  if (!waterOk) { relayWrite(RLY_FAN, false); return; }

  bool on = relayState[RLY_FAN];
  if (on) on = (waterTemp > (cfg.fanOn - cfg.fanHyst));
  else    on = (waterTemp > (cfg.fanOn + cfg.fanHyst));
  if (alarms & AL_OVERHEAT) on = true;             // при перегреве дуем всегда
  relayWrite(RLY_FAN, on);
}

static void ledWrite(uint8_t ch, uint8_t pct) {
  if (ch >= LED_CHANNELS) return;
  if (pct > 100) pct = 100;
  ledPct[ch] = pct;
  uint8_t out = gammaLut[pct];
  if (out == ledOut[ch]) return;
  ledOut[ch] = out;
  analogWrite(LED_PINS[ch], out);
}

/* Сколько минут ночник горит после заката.
   Считается как «ночник до» минус время заката с переходом через полночь.
   0 = ночник не включается (гаснет сразу после заката). */
static uint16_t nightLenMinutes() {
  int16_t n = (int16_t)((int16_t)cfg.nightOffHour * 60 + cfg.nightOffMin) -
              (int16_t)((int16_t)cfg.offHour * 60 + cfg.offMin);
  n %= 1440;
  if (n < 0) n += 1440;
  return (uint16_t)n;
}

/* smoothstep: плавный старт и финиш рассвета/заката */
static float smoothstep01(float k) {
  k = clampf(k, 0.0f, 1.0f);
  return k * k * (3.0f - 2.0f * k);
}

static void lightTask() {
  static uint32_t lastRun = 0;
  uint32_t now = millis();
  if (now - lastRun < 500) return;                 // достаточно 2 раза в секунду
  lastRun = now;

  /* режим «Ночник»: держим установленную яркость ночника независимо от времени */
  if (cfg.lightMode == 2) {
    lightPhase = LP_NIGHT;
    for (uint8_t i = 0; i < LED_CHANNELS; i++) ledWrite(i, cfg.nightPct[i]);
    return;
  }

  /* ручной режим: яркость задаётся ползунками */
  if (cfg.lightMode == 1) {
    lightPhase = LP_MANUAL;
    for (uint8_t i = 0; i < LED_CHANNELS; i++) ledWrite(i, cfg.manualPct[i]);
    return;
  }

  DateTime dt;
  epochToLocal(nowEpoch(), dt);
  bool dayEnabled = ((cfg.daysMask >> dt.weekday) & 0x01) != 0;

  int16_t nowMin = (int16_t)dt.hour * 60 + dt.minute;
  int16_t onMin  = (int16_t)cfg.onHour * 60 + cfg.onMin;
  int16_t offMin = (int16_t)cfg.offHour * 60 + cfg.offMin;
  int16_t dur = (int16_t)(offMin - onMin);
  if (dur <= 0) dur += 1440;
  int16_t el = (int16_t)(nowMin - onMin);
  if (el < 0) el += 1440;

  uint8_t pct[LED_CHANNELS];
  uint8_t ramp = cfg.rampMin < 1 ? 1 : cfg.rampMin;
  int16_t nightLen = (int16_t)nightLenMinutes();   // сколько горит ночник после заката

  if (!dayEnabled) {                               // этот день недели отключён — света нет
    lightPhase = LP_NIGHT;
    for (uint8_t i = 0; i < LED_CHANNELS; i++) pct[i] = 0;
  } else if (el >= dur) {                          // ночь
    int16_t afterOff = (int16_t)(el - dur);        // минут прошло после заката
    if (nightLen > 0 && afterOff < nightLen) {     // ещё горит ночник
      lightPhase = LP_NIGHT;
      for (uint8_t i = 0; i < LED_CHANNELS; i++) pct[i] = cfg.nightPct[i];
    } else {                                       // ночник погас — полная темнота до утра
      lightPhase = LP_NIGHT;
      for (uint8_t i = 0; i < LED_CHANNELS; i++) pct[i] = 0;
    }
  } else if (el < ramp) {                          // рассвет
    lightPhase = LP_DAWN;
    float s = smoothstep01((float)el / (float)ramp);
    for (uint8_t i = 0; i < LED_CHANNELS; i++)
      pct[i] = (uint8_t)((float)cfg.dayPct[i] * s + 0.5f);
  } else if (el >= (int16_t)(dur - ramp)) {        // закат
    lightPhase = LP_DUSK;
    float s = smoothstep01((float)(dur - el) / (float)ramp);
    for (uint8_t i = 0; i < LED_CHANNELS; i++)
      pct[i] = (uint8_t)((float)cfg.dayPct[i] * s + 0.5f);
  } else {                                         // день
    lightPhase = LP_DAY;
    for (uint8_t i = 0; i < LED_CHANNELS; i++) pct[i] = cfg.dayPct[i];
  }

  for (uint8_t i = 0; i < LED_CHANNELS; i++) ledWrite(i, pct[i]);
}

/* ====================== ИСТОРИЯ ====================== */
static void historyPush() {
  HistPoint& p = hist[histNext];
  p.t100 = waterOk ? (int16_t)lroundf(waterTemp * 100.0f) : 0;
  p.a100 = airOk ? (int16_t)lroundf(airTemp * 100.0f) : 0;
  p.flags = 0;
  if (relayState[RLY_HEAT]) p.flags |= 0x01;
  if (relayState[RLY_FAN])  p.flags |= 0x02;
  if (relayState[RLY_PUMP]) p.flags |= 0x04;
  if (relayState[RLY_AIR])  p.flags |= 0x08;
  bool lightOn = false;
  for (uint8_t i = 0; i < LED_CHANNELS; i++) if (ledPct[i] > 2) lightOn = true;
  if (lightOn) p.flags |= 0x10;

  histNext = (uint16_t)((histNext + 1) % HISTORY_POINTS);
  if (histCount < HISTORY_POINTS) histCount++;
  histNum++;
}

/* ====================== СЕТЬ ====================== */
static const char* wlStatusName(uint8_t st) {
  switch (st) {
    case WL_IDLE_STATUS:     return "ожидание";
    case WL_NO_SSID_AVAIL:   return "такая сеть не найдена (только 2.4 ГГц!)";
    case WL_SCAN_COMPLETED:  return "сканирование";
    case WL_CONNECTED:       return "подключено";
    case WL_CONNECT_FAILED:  return "ошибка подключения (проверьте пароль)";
    case WL_CONNECTION_LOST: return "связь потеряна";
    case WL_DISCONNECTED:    return "не подключено";
    case WL_AP_LISTENING:    return "точка доступа работает";
    case WL_AP_CONNECTED:    return "клиент подключён к точке доступа";
    case WL_AP_FAILED:       return "ошибка запуска точки доступа";
    default:                 return "неизвестный статус";
  }
}

/* Большой баннер с адресом: печатается после подключения, в режиме точки
   доступа и каждый раз, когда открывают монитор порта. */
static void printNetBanner(const char* why) {
  Serial.println();
  Serial.println(F("============================================================"));
  Serial.print(F("  ВЕБ-СЕРВЕР РАБОТАЕТ — "));
  Serial.println(why);
  Serial.println(F("------------------------------------------------------------"));
  Serial.print(F("  Откройте в браузере:   http://"));
  Serial.println(ipStr);
  if (apMode) {
    Serial.print(F("  Wi-Fi точка доступа:   "));
    Serial.println(AP_SSID);
    Serial.print(F("  пароль:                "));
    Serial.println(AP_PASS);
    Serial.println(F("  (адрес 192.168.4.1 одинаков всегда — он у самой платы)"));
  } else {
    Serial.print(F("  плата видна в роутере как \""));
    Serial.print(WIFI_HOSTNAME);
    Serial.println(F("\", часто работает http://" WIFI_HOSTNAME ".local"));
    Serial.println(F("  если адрес не подходит — отправьте в это окно букву 'a',"));
    Serial.println(F("  плата поднимет свою точку доступа Aquarium"));
  }
  Serial.println(F("============================================================"));
  Serial.println();
}

static void printConfigSummary() {
  char b[24];
  Serial.println(F("--- конфигурация ---"));
  Serial.print(F("  прошивка        : ")); Serial.print(F(FW_NAME));
  Serial.print(F(" v")); Serial.println(F(FW_VERSION));
  Serial.print(F("  Wi-Fi сеть      : ")); Serial.println(WIFI_SSID);
  if (strcmp(WIFI_SSID, WIFI_SSID_DEFAULT) == 0)
    Serial.println(F("  ! сеть не изменена в config.h — сразу поднимется точка доступа"));
  Serial.print(F("  часовой пояс    : ")); Serial.print((int)TZ_OFFSET_MIN / 60);
  Serial.print(F(" ч ")); Serial.print(abs((int)TZ_OFFSET_MIN % 60));
  Serial.print(F(" мин, NTP ")); Serial.println(F(NTP_SERVER));
#if USE_STATIC_IP
  Serial.println(F("  адрес           : статический " ));
#else
  Serial.println(F("  адрес           : по DHCP от роутера"));
#endif
  Serial.print(F("  реле            : помпа D")); Serial.print(PIN_RELAY_PUMP);
  Serial.print(F(", компрессор D")); Serial.print(PIN_RELAY_AIR);
  Serial.print(F(", вентилятор D")); Serial.print(PIN_RELAY_FAN);
  Serial.print(F(", нагреватель D")); Serial.println(PIN_RELAY_HEAT);
  Serial.print(F("  светильник      : D")); Serial.print(LED_PINS[0]);
  Serial.print(F(", каналов ")); Serial.print((unsigned)LED_CHANNELS);
  snprintf(b, sizeof(b), ", %02u:%02u-%02u:%02u", cfg.onHour, cfg.onMin, cfg.offHour, cfg.offMin);
  Serial.print(b);
  Serial.print(F(", переход ")); Serial.print((unsigned)cfg.rampMin); Serial.print(F(" мин"));
  Serial.print(F(", день ")); Serial.print((unsigned)cfg.dayPct[0]); Serial.print(F("%"));
  Serial.print(F(", ночник ")); Serial.print((unsigned)cfg.nightPct[0]); Serial.print(F("%"));
  snprintf(b, sizeof(b), " до %02u:%02u", cfg.nightOffHour, cfg.nightOffMin);
  Serial.print(b);
  Serial.print(F(" (")); Serial.print((unsigned)(nightLenMinutes() / 60));
  Serial.print(F(" ч), дальше темнота"));
  Serial.println();
  Serial.print(F("  термостат       : "));
  fmtF(b, sizeof(b), cfg.setpoint, 1); Serial.print(b);
  Serial.print(F(" ± ")); fmtF(b, sizeof(b), cfg.hyst, 1); Serial.print(b);
  Serial.print(F(" °C, аварийный порог ")); fmtF(b, sizeof(b), cfg.maxWaterTemp, 1); Serial.print(b);
  Serial.print(F(" °C, ")); fmtF(b, sizeof(b), cfg.heaterPowerW, 0); Serial.print(b); Serial.println(F(" Вт"));
  Serial.print(F("  вентилятор      : включать выше "));
  fmtF(b, sizeof(b), cfg.fanOn, 1); Serial.print(b); Serial.println(F(" °C"));
  Serial.print(F("  настройки       : "));
  Serial.println(settingsFromNvm ? F("загружены из флеш-памяти") : F("по умолчанию"));
  Serial.println();
}

static void printSensorList() {
  uint8_t n = ds.getDeviceCount();
  Serial.print(F("[датчики] найдено устройств 1-Wire: ")); Serial.println((unsigned)n);
  DeviceAddress a;
  for (uint8_t i = 0; i < n; i++) {
    if (!ds.getAddress(a, i)) continue;
    Serial.print(F("  №")); Serial.print((unsigned)i); Serial.print(F("  "));
    for (uint8_t k = 0; k < 8; k++) {
      if (a[k] < 0x10) Serial.print('0');
      Serial.print(a[k], HEX);
    }
    if (i == dsWaterIdx)     Serial.print(F("  → вода (по нему работает термостат)"));
    else if (i == dsAirIdx)  Serial.print(F("  → воздух"));
    Serial.println();
  }
  if (n == 0)
    Serial.println(F("  ! датчиков нет: проверьте резистор 4.7 кОм на 3V3 и провод DQ → D7"));
  else if (dsAirIdx == 0xFF)
    Serial.println(F("  ! второй датчик (воздух) не найден — предупреждение в интерфейсе"));
}

static void printHeartbeat() {
  char b[24];
  uint32_t up = millis() / 1000UL;
  DateTime dt; epochToLocal(nowEpoch(), dt);

  Serial.println();
  snprintf(b, sizeof(b), "[отч] %02u:%02u:%02u", dt.hour, dt.minute, dt.second);
  Serial.print(b);
  Serial.print(F("  вода "));
  fmtF(b, sizeof(b), waterOk ? waterTemp : NAN, 2); Serial.print(b);
  Serial.print(F("°C  воздух "));
  fmtF(b, sizeof(b), airOk ? airTemp : NAN, 2); Serial.print(b);
  Serial.print(F("°C  уставка "));
  fmtF(b, sizeof(b), cfg.setpoint, 1); Serial.print(b);
  Serial.println(F("°C"));
  Serial.print(F("      реле: помпа "));   Serial.print(relayState[RLY_PUMP] ? F("ВКЛ") : F("выкл"));
  Serial.print(F("  компрессор "));        Serial.print(relayState[RLY_AIR] ? F("ВКЛ") : F("выкл"));
  Serial.print(F("  вентилятор "));        Serial.print(relayState[RLY_FAN] ? F("ВКЛ") : F("выкл"));
  Serial.print(F("  нагрев "));            Serial.print(relayState[RLY_HEAT] ? F("ВКЛ") : F("выкл"));
  Serial.print(F("  свет "));              Serial.print((unsigned)ledPct[0]);
  Serial.print(F("% (")); Serial.print(PHASE_NAMES[(int)lightPhase]); Serial.println(')');
  Serial.print(F("      http://")); Serial.print(ipStr);
  Serial.print(apMode ? F("  (точка доступа)") : F(""));
  Serial.print(F("  RSSI ")); Serial.print((long)rssi);
  Serial.print(F(" dBm  работает ")); Serial.print(up / 3600UL);
  Serial.print(F(" ч ")); Serial.print((up % 3600UL) / 60UL);
  Serial.print(F(" мин  запросов ")); Serial.println((unsigned long)httpRequests);
  if (alarms) { Serial.print(F("      ТРЕВОГИ: ")); Serial.println(alarmText); }
  if (!waterOk) Serial.println(F("      ! нет данных о температуре воды — нагрев заблокирован"));
}

static void logAlarms() {
  if (alarms == lastAlarmBits) return;
  if (alarms & (uint8_t)~lastAlarmBits) {
    Serial.print(F("[тревога] "));
    Serial.println(alarmText[0] ? alarmText : "сработала защита");
  } else {
    Serial.println(F("[тревога] снята"));
  }
  lastAlarmBits = alarms;
}

/* ---------- сторожевой таймер SAMD21 ---------- */
static uint8_t readResetCause() {
  /* читаем биты масками: имена битовых полей (RCAUSE.bit.WDT) конфликтуют
     с макросом WDT из заголовков SAMD21 */
  uint8_t v = (uint8_t)PM->RCAUSE.reg;
  uint8_t c = 0;
  if (v & PM_RCAUSE_POR)  c |= 0x01;
  if (v & PM_RCAUSE_EXT)  c |= 0x02;
  if (v & PM_RCAUSE_WDT)  c |= 0x04;
  if (v & PM_RCAUSE_SYST) c |= 0x08;
  return c;
}

static const char* resetCauseName(uint8_t bits) {
  if (bits & 0x04) return "сторожевой таймер (прошивка зависла)";
  if (bits & 0x01) return "включение питания";
  if (bits & 0x02) return "кнопка RESET или внешний сигнал";
  if (bits & 0x08) return "программная перезагрузка";
  return "неизвестно";
}

static void wdtBegin() {
#if WATCHDOG_ENABLE
  WDT->CTRL.bit.ENABLE = 0;                         // настраивать можно только выключенным
  while (WDT->STATUS.bit.SYNCBUSY) { }
  WDT->CONFIG.bit.PER = WDT_CONFIG_PER_16K_Val;     // 16384 цикла по 1/1024 с ≈ 16 с
  while (WDT->STATUS.bit.SYNCBUSY) { }
  WDT->CTRL.bit.ENABLE = 1;
  while (WDT->STATUS.bit.SYNCBUSY) { }
  Serial.print(F("[wdt] сторожевой таймер включён, период "));
  Serial.print((int)WATCHDOG_PERIOD_S);
  Serial.println(F(" с — при зависании плата перезагрузится сама"));
#endif
}

static void wdtFeed() {
#if WATCHDOG_ENABLE
  WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
#endif
}

/* ---------- подключение к роутеру ---------- */
static void connectSta() {
#if USE_STATIC_IP
  IPAddress ip(STATIC_IP), gw(STATIC_GW), mask(STATIC_MASK), dns(STATIC_DNS);
  WiFi.config(ip, dns, gw, mask);
#endif
  WiFi.setHostname(WIFI_HOSTNAME);
  Serial.print(F("[wifi] подключаюсь к сети \""));
  Serial.print(WIFI_SSID);
  Serial.print(F("\" ... "));
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static void startAP() {
  Serial.println();
  Serial.print(F("[wifi] поднимаю точку доступа \""));
  Serial.print(AP_SSID);
  Serial.println(F("\""));
  WiFi.end();
  WiFi.beginAP(AP_SSID, AP_PASS);
  apMode = true;
  apNextTry = millis() + 5UL * 60UL * 1000UL;
  IPAddress ip = WiFi.localIP();
  snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  snprintf(gwStr, sizeof(gwStr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  rssi = 0;
  if (ip[0] == 0) snprintf(ipStr, sizeof(ipStr), "192.168.4.1");
  Serial.print(F("[wifi] адрес точки доступа: ")); Serial.println(ipStr);
  Serial.print(F("[wifi] подключитесь к Wi-Fi \"")); Serial.print(AP_SSID);
  Serial.print(F("\" с паролем \"")); Serial.print(AP_PASS); Serial.println(F("\""));
  printNetBanner("режим точки доступа");
  bannerShown = true;
  bannerValid = true;
}

static void wifiTask() {
  static uint32_t lastCheck = 0;
  uint32_t now = millis();
  if (now - lastCheck < 1000) return;
  lastCheck = now;

  uint8_t st = WiFi.status();

  if (!apMode) {
    if (st == WL_CONNECTED) {
      IPAddress ip = WiFi.localIP();
      if (ip[0] == 0) return;                     // DHCP ещё не выдал адрес
      snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
      IPAddress gw = WiFi.gatewayIP();
      snprintf(gwStr, sizeof(gwStr), "%u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);
      rssi = WiFi.RSSI();
      wifiWasConnected = true;
      wasInAp = false;
      staLostSince = 0;
      if (!bannerValid) {
        Serial.println();
        Serial.print(F("[wifi] подключено к \"")); Serial.print(WiFi.SSID());
        Serial.print(F("\", IP ")); Serial.print(ipStr);
        Serial.print(F(", RSSI ")); Serial.print((long)rssi); Serial.println(F(" dBm"));
        printNetBanner("подключение к роутеру выполнено");
        bannerShown = true;
        bannerValid = true;
      }
      lastWlStatus = st;
      return;
    }

    if (st != lastWlStatus) {
      Serial.print(F("[wifi] статус: "));
      Serial.print(wlStatusName(st));
      if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL || st == WL_CONNECTION_LOST) {
        Serial.print(F(" (код ")); Serial.print((unsigned)WiFi.reasonCode()); Serial.print(')');
      }
      Serial.println();
      lastWlStatus = st;
      bannerValid = false;
    }

    if (wifiWasConnected) {                       // связь была и потерялась
      if (staLostSince == 0) staLostSince = now;
#if AP_FALLBACK
      if ((now - staLostSince) > AP_ON_LOST_MS) {
        Serial.println(F("[wifi] связи нет больше 5 минут — поднимаю точку доступа"));
        startAP();
        return;
      }
#endif
    }

    if (wasInAp && (int32_t)(now - apRetryUntil) >= 0) {
      Serial.println(F("[wifi] роутер не ответил — возвращаюсь в режим точки доступа"));
      wasInAp = false;
      startAP();
      return;
    }

    if (now < wifiNextTry) return;
    wifiNextTry = now + WIFI_RETRY_MS;
    if (!wifiWasConnected && now > STA_TIMEOUT_MS) {
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
    connectSta();
    return;
  }

  /* режим точки доступа: раз в 5 минут пробуем вернуться в сеть */
  if ((int32_t)(now - apNextTry) >= 0) {
    apNextTry = now + 5UL * 60UL * 1000UL;
    Serial.println(F("[wifi] из точки доступа пробую роутер (пауза до 1 минуты)"));
    wasInAp = true;
    apMode = false;
    bannerValid = false;
    staLostSince = 0;
    apRetryUntil = now + 60UL * 1000UL;
    connectSta();
  }
}

/* ====================== HTTP ====================== */
static bool readLine(WiFiClient& c, char* buf, size_t maxLen) {
  size_t n = 0;
  uint32_t t0 = millis();
  while ((millis() - t0) < HTTP_TIMEOUT_MS) {
    wdtFeed();                 // медленный клиент не должен вызывать перезагрузку
    if (!c.available()) {
      if (!c.connected()) break;
      delay(1);
      continue;
    }
    int ch = c.read();
    if (ch < 0) continue;
    if (ch == '\n') { buf[n] = 0; return true; }
    if (ch != '\r' && n < (maxLen - 1)) buf[n++] = (char)ch;
  }
  buf[n] = 0;
  return n > 0;
}

static void streamHead(WiFiClient& c, const char* status, const char* ctype) {
  c.print(F("HTTP/1.1 "));
  c.print(status);
  c.print(F("\r\nContent-Type: "));
  c.print(ctype);
  c.print(F("\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"));
}

static void sendPage(WiFiClient& c) {
  uint32_t len = (uint32_t)(sizeof(INDEX_HTML) - 1);
  c.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"));
  c.print(F("Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "));
  c.print(len);
  c.print(F("\r\n\r\n"));
  uint32_t off = 0;
  while (off < len) {
    uint32_t chunk = len - off;
    if (chunk > 1024) chunk = 1024;
    c.write((const uint8_t*)(INDEX_HTML + off), chunk);
    off += chunk;
    wdtFeed();                 // страница большая, клиент может быть медленным
    delay(2);
  }
  c.flush();
}

/* отправка JSON: заголовок без Content-Length, тело до закрытия соединения */
static void jsonHead(WiFiClient& c, const char* status) {
  streamHead(c, status, "application/json; charset=utf-8");
}

static void sendState(WiFiClient& c) {
  char b[160];
  static char cyc[900];
  size_t p = 0;
  cyc[0] = 0;

  for (uint8_t k = 0; k < cycleCount; k++) {
    uint8_t idx = (uint8_t)((cycleNext + CYCLE_LOG_SIZE - 1 - k) % CYCLE_LOG_SIZE);
    CycleLog& cl = cycles[idx];
    char ts[8] = "--:--";
    if (cl.startEpoch > 1000000UL) {
      DateTime dt; epochToLocal(cl.startEpoch, dt);
      snprintf(ts, sizeof(ts), "%02u:%02u", dt.hour, dt.minute);
    }
    int rem = (int)sizeof(cyc) - (int)p;
    if (rem < 90) break;
    int w = snprintf(cyc + p, rem,
                     "%s{\"t\":\"%s\",\"ts\":%d.%02d,\"te\":%d.%02d,\"d\":%u,\"sp\":%d.%02d}",
                     k ? "," : "", ts,
                     cl.tStart100 / 100, abs(cl.tStart100 % 100),
                     cl.tEnd100 / 100, abs(cl.tEnd100 % 100),
                     (unsigned)cl.durSec,
                     cl.set100 / 100, abs(cl.set100 % 100));
    if (w <= 0) break;
    p += (size_t)w;
  }

  jsonHead(c, "200 OK");
  c.print(F("{\"ok\":1,\"fw\":\"" FW_VERSION "\",\"name\":\"" FW_NAME "\",\"up\":"));
  c.print((unsigned long)(millis() / 1000UL));

  /* время и сеть */
  DateTime dt; epochToLocal(nowEpoch(), dt);
  snprintf(b, sizeof(b), ",\"time\":\"%02u:%02u:%02u\",\"date\":\"%02u.%02u.%04u\",\"wd\":%u",
           dt.hour, dt.minute, dt.second, dt.day, dt.month, dt.year, dt.weekday);
  c.print(b);
  snprintf(b, sizeof(b), ",\"tsrc\":\"%s\",\"ip\":\"%s\",\"ap\":%u,\"rssi\":%ld",
           timeFromNtp ? "ntp" : "build", ipStr, apMode ? 1 : 0, (long)rssi);
  c.print(b);
  snprintf(b, sizeof(b), ",\"gw\":\"%s\",\"ssid\":\"%s\",\"nvm\":%u,\"rstrt\":%u",
           gwStr, apMode ? AP_SSID : WIFI_SSID, settingsFromNvm ? 1 : 0, (unsigned)resetCauseBits);
  c.print(b);
  snprintf(b, sizeof(b), ",\"reqs\":%lu,\"cmds\":%lu,\"wd\":%u,\"host\":\"%s\"",
           (unsigned long)httpRequests, (unsigned long)httpCmds,
           (unsigned)(WATCHDOG_ENABLE ? 1 : 0), WIFI_HOSTNAME);
  c.print(b);

  /* температуры */
  fmtF(b, sizeof(b), waterOk ? waterTemp : NAN, 2);
  c.print(F(",\"t\":")); c.print(b);
  fmtF(b, sizeof(b), airOk ? airTemp : NAN, 2);
  c.print(F(",\"ta\":")); c.print(b);
  fmtF(b, sizeof(b), waterShown, 2);
  c.print(F(",\"tf\":")); c.print(b);
  snprintf(b, sizeof(b), ",\"tok\":%u,\"aok\":%u,\"nsen\":%u",
           waterOk ? 1 : 0, airOk ? 1 : 0, (unsigned)ds.getDeviceCount());
  c.print(b);

  /* тревоги */
  snprintf(b, sizeof(b), ",\"al\":%u,\"altext\":\"%s\"", (unsigned)alarms, alarmText);
  c.print(b);

  /* реле */
  snprintf(b, sizeof(b),
           ",\"pump\":%u,\"air\":%u,\"fan\":%u,\"heat\":%u,\"hlock\":%u,\"hl\":%lu",
           relayState[RLY_PUMP] ? 1 : 0, relayState[RLY_AIR] ? 1 : 0,
           relayState[RLY_FAN] ? 1 : 0, relayState[RLY_HEAT] ? 1 : 0,
           heaterLocked ? 1 : 0,
           (unsigned long)(relayState[RLY_HEAT] ? (millis() - heaterOnAt) / 1000UL : 0));
  c.print(b);
  snprintf(b, sizeof(b), ",\"fanman\":%u,\"heatman\":%u", fanManual ? 1 : 0, heatManual ? 1 : 0);
  c.print(b);

  /* термостат */
  fmtF(b, sizeof(b), cfg.setpoint, 1);   c.print(F(",\"sp\":"));  c.print(b);
  fmtF(b, sizeof(b), cfg.hyst, 2);       c.print(F(",\"hy\":"));  c.print(b);
  fmtF(b, sizeof(b), cfg.maxWaterTemp, 1); c.print(F(",\"maxt\":")); c.print(b);
  fmtF(b, sizeof(b), cfg.fanOn, 1);      c.print(F(",\"fanon\":")); c.print(b);
  fmtF(b, sizeof(b), cfg.fanHyst, 2);    c.print(F(",\"fanhy\":")); c.print(b);
  fmtF(b, sizeof(b), cfg.heaterPowerW, 0); c.print(F(",\"pw\":")); c.print(b);

  /* энергия и подсказка */
  snprintf(b, sizeof(b), ",\"kwh\":%lu,\"kwhd\":%lu,\"est\":" ,
           (unsigned long)((cfg.heaterPowerW * (float)heatMsTotal / 3600000.0f) * 100.0f),
           (unsigned long)((cfg.heaterPowerW * (float)heatMsToday / 3600000.0f) * 100.0f));
  c.print(b);
  c.print((unsigned long)(waterOk ? estimateHeaterSeconds(clampf(cfg.setpoint - waterTemp, 0.0f, 10.0f)) : 0));

  /* свет */
  snprintf(b, sizeof(b), ",\"lmod\":%u,\"phase\":\"%s\",\"lp\":[",
           (unsigned)cfg.lightMode, PHASE_NAMES[(int)lightPhase]);
  c.print(b);
  for (uint8_t i = 0; i < LED_CHANNELS; i++) {
    snprintf(b, sizeof(b), "%s%u", i ? "," : "", (unsigned)ledPct[i]);
    c.print(b);
  }
  c.print(F("],\"day\":["));
  for (uint8_t i = 0; i < LED_CHANNELS; i++) {
    snprintf(b, sizeof(b), "%s%u", i ? "," : "", (unsigned)cfg.dayPct[i]);
    c.print(b);
  }
  c.print(F("],\"night\":["));
  for (uint8_t i = 0; i < LED_CHANNELS; i++) {
    snprintf(b, sizeof(b), "%s%u", i ? "," : "", (unsigned)cfg.nightPct[i]);
    c.print(b);
  }
  c.print(F("],\"man\":["));
  for (uint8_t i = 0; i < LED_CHANNELS; i++) {
    snprintf(b, sizeof(b), "%s%u", i ? "," : "", (unsigned)cfg.manualPct[i]);
    c.print(b);
  }
  c.print(F("],\"labels\":["));
  for (uint8_t i = 0; i < LED_CHANNELS; i++) {
    c.print('"'); c.print(LED_LABELS[i]); c.print('"');
    if (i + 1 < LED_CHANNELS) c.print(',');
  }
  c.print(F("],\"nch\":"));
  c.print((unsigned)LED_CHANNELS);

  /* расписание */
  snprintf(b, sizeof(b), ",\"on\":\"%02u:%02u\",\"off\":\"%02u:%02u\",\"ramp\":%u,\"days\":%u",
           cfg.onHour, cfg.onMin, cfg.offHour, cfg.offMin, (unsigned)cfg.rampMin,
           (unsigned)cfg.daysMask);
  c.print(b);
  snprintf(b, sizeof(b), ",\"noff\":\"%02u:%02u\",\"nlen\":%u",
           cfg.nightOffHour, cfg.nightOffMin, (unsigned)nightLenMinutes());
  c.print(b);

  /* журнал циклов */
  c.print(F(",\"cyc\":["));
  c.print(cyc);
  c.print(F("]}"));
  c.flush();
}

static void sendHistory(WiFiClient& c) {
  char b[40];
  jsonHead(c, "200 OK");
  snprintf(b, sizeof(b), "{\"n\":%u,\"p\":%u,\"t\":[", (unsigned)histCount,
           (unsigned)(HISTORY_PERIOD_MS / 1000UL));
  c.print(b);

  uint16_t start = (uint16_t)((histNext + HISTORY_POINTS - histCount) % HISTORY_POINTS);
  for (uint16_t k = 0; k < histCount; k++) {
    HistPoint& p = hist[(start + k) % HISTORY_POINTS];
    snprintf(b, sizeof(b), "%s%d", k ? "," : "", p.t100);
    c.print(b);
  }
  c.print(F("],\"a\":["));
  for (uint16_t k = 0; k < histCount; k++) {
    HistPoint& p = hist[(start + k) % HISTORY_POINTS];
    snprintf(b, sizeof(b), "%s%d", k ? "," : "", p.a100);
    c.print(b);
  }
  c.print(F("],\"f\":["));
  for (uint16_t k = 0; k < histCount; k++) {
    HistPoint& p = hist[(start + k) % HISTORY_POINTS];
    snprintf(b, sizeof(b), "%s%u", k ? "," : "", (unsigned)p.flags);
    c.print(b);
  }
  c.print(F("]}"));
  c.flush();
}

static void sendScan(WiFiClient& c) {
  jsonHead(c, "200 OK");
  char b[40];
  int n = (int)ds.getDeviceCount();
  c.print(F("{\"ok\":1,\"n\":"));
  c.print(n);
  c.print(F(",\"water\":"));
  c.print((unsigned)dsWaterIdx);
  c.print(F(",\"dev\":["));
  DeviceAddress a;
  for (int i = 0; i < n; i++) {
    if (!ds.getAddress(a, (uint8_t)i)) continue;
    c.print(i ? "," : "");
    c.print('"');
    for (uint8_t k = 0; k < 8; k++) {
      snprintf(b, sizeof(b), "%02X", a[k]);
      c.print(b);
    }
    c.print('"');
  }
  c.print(F("]}"));
  c.flush();
}

/* --- разбор параметров запроса --- */
static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool httpParam(const char* q, const char* key, char* out, size_t outLen) {
  if (!q || !*q || outLen < 2) return false;
  size_t klen = strlen(key);
  const char* p = q;
  while (p && *p) {
    if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char* v = p + klen + 1;
      size_t n = 0;
      while (*v && *v != '&' && n < (outLen - 1)) {
        char ch = *v++;
        if (ch == '%' && v[0] && v[1]) {
          int hi = hexVal(v[0]), lo = hexVal(v[1]);
          if (hi >= 0 && lo >= 0) { ch = (char)(hi * 16 + lo); v += 2; }
        } else if (ch == '+') {
          ch = ' ';
        }
        out[n++] = ch;
      }
      out[n] = 0;
      return true;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  return false;
}

static long httpInt(const char* q, const char* key, long def) {
  char buf[24];
  if (!httpParam(q, key, buf, sizeof(buf))) return def;
  return strtol(buf, NULL, 10);
}

static float httpFloat(const char* q, const char* key, float def) {
  char buf[24];
  if (!httpParam(q, key, buf, sizeof(buf))) return def;
  const char* s = buf;
  bool neg = false;
  if (*s == '-') { neg = true; s++; } else if (*s == '+') s++;
  float v = 0.0f; bool any = false;
  while (*s >= '0' && *s <= '9') { v = v * 10.0f + (float)(*s - '0'); s++; any = true; }
  if (*s == '.' || *s == ',') {
    s++;
    float m = 0.1f;
    while (*s >= '0' && *s <= '9') { v += (float)(*s - '0') * m; m *= 0.1f; s++; any = true; }
  }
  if (!any) return def;
  return neg ? -v : v;
}

static void buildAlarmText() {
  alarmText[0] = 0;
  if (alarms & AL_WATER_SENSOR)  strncat(alarmText, "Нет датчика воды! ", sizeof(alarmText) - strlen(alarmText) - 1);
  if (alarms & AL_OVERHEAT)      strncat(alarmText, "Перегрев! ", sizeof(alarmText) - strlen(alarmText) - 1);
  if (alarms & AL_HEAT_TIMEOUT)  strncat(alarmText, "Нагрев выключен по таймеру. ", sizeof(alarmText) - strlen(alarmText) - 1);
  if (alarms & AL_AIR_SENSOR)    strncat(alarmText, "Нет датчика воздуха. ", sizeof(alarmText) - strlen(alarmText) - 1);
  if (alarms & AL_NO_TIME)       strncat(alarmText, "Время не по NTP. ", sizeof(alarmText) - strlen(alarmText) - 1);
}

static void handleCmd(const char* q, char* reply, size_t replyLen) {
  char val[32];
  bool ok = true;
  const char* err = "";

  /* ---------- реле ---------- */
  static const char* const RELAY_KEYS[4] = { "pump", "air", "fan", "heat" };
  for (uint8_t i = 0; i < 4; i++) {
    if (!httpParam(q, RELAY_KEYS[i], val, sizeof(val))) continue;
    bool on;
    if (strcmp(val, "toggle") == 0) on = !relayState[i];
    else on = (strcmp(val, "1") == 0 || strcmp(val, "on") == 0);

    if (i == RLY_HEAT) {
      if (on && !waterOk) { ok = false; err = "нет датчика воды"; break; }
      if (on && waterTemp > cfg.maxWaterTemp) { ok = false; err = "перегрев"; break; }
      heatManual = true;
      heatManualState = on;
      heatManualUntil = millis() + 15UL * 60UL * 1000UL;   // ручное включение на 15 минут
      if (!on) { heatManual = false; relayWrite(RLY_HEAT, false); heaterOffAt = millis(); }
    } else if (i == RLY_FAN) {
      fanManual = true;
      fanManualState = on;
      relayWrite(RLY_FAN, on);
    } else {
      relayWrite(i, on);
      if (i == RLY_PUMP) cfg.pumpOn = on ? 1 : 0;
      if (i == RLY_AIR)  cfg.airOn  = on ? 1 : 0;
      settingsMarkDirty();
    }
  }

  /* вентилятор обратно в авто */
  if (httpParam(q, "fanauto", val, sizeof(val)) && atoi(val)) {
    fanManual = false;
  }
  if (httpParam(q, "heatauto", val, sizeof(val)) && atoi(val)) {
    heatManual = false;
  }

  /* ---------- термостат ---------- */
  if (httpParam(q, "set", val, sizeof(val))) {
    float v = httpFloat(q, "set", cfg.setpoint);
    cfg.setpoint = clampf(v, 15.0f, cfg.maxWaterTemp - 0.5f);
    settingsMarkDirty();
  }
  if (httpParam(q, "hyst", val, sizeof(val))) {
    cfg.hyst = clampf(httpFloat(q, "hyst", cfg.hyst), 0.1f, 2.0f);
    settingsMarkDirty();
  }
  if (httpParam(q, "maxt", val, sizeof(val))) {
    cfg.maxWaterTemp = clampf(httpFloat(q, "maxt", cfg.maxWaterTemp), 20.0f, 40.0f);
    if (cfg.setpoint > cfg.maxWaterTemp - 0.5f) cfg.setpoint = cfg.maxWaterTemp - 0.5f;
    settingsMarkDirty();
  }
  if (httpParam(q, "fanon", val, sizeof(val))) {
    cfg.fanOn = clampf(httpFloat(q, "fanon", cfg.fanOn), 15.0f, 40.0f);
    settingsMarkDirty();
  }
  if (httpParam(q, "fanhy", val, sizeof(val))) {
    cfg.fanHyst = clampf(httpFloat(q, "fanhy", cfg.fanHyst), 0.05f, 2.0f);
    settingsMarkDirty();
  }
  if (httpParam(q, "pw", val, sizeof(val))) {
    cfg.heaterPowerW = clampf(httpFloat(q, "pw", cfg.heaterPowerW), 5.0f, 1000.0f);
    settingsMarkDirty();
  }

  /* ---------- свет ---------- */
  if (httpParam(q, "lmode", val, sizeof(val))) {
    if (strcmp(val, "auto") == 0)        cfg.lightMode = 0;   // рассвет-день-закат-ночник
    else if (strcmp(val, "manual") == 0) cfg.lightMode = 1;   // ручная яркость
    else if (strcmp(val, "night") == 0)  cfg.lightMode = 2;   // ночник включён сейчас
    settingsMarkDirty();
  }
  if (httpParam(q, "lch", val, sizeof(val))) {
    long ch = httpInt(q, "lch", 0);
    long pct = httpInt(q, "lval", 0);
    if (ch >= 0 && ch < LED_CHANNELS) {
      cfg.manualPct[ch] = (uint8_t)clampf((float)pct, 0.0f, 100.0f);
      if (cfg.lightMode == 0) cfg.lightMode = 1;     // ручная правка = ручной режим
      settingsMarkDirty();
    }
  }
  if (httpParam(q, "lday", val, sizeof(val))) {      // lday=<канал>,<процент>
    char* comma = strchr(val, ',');
    if (comma) {
      *comma = 0;
      long ch = atol(val), pct = atol(comma + 1);
      if (ch >= 0 && ch < LED_CHANNELS) {
        cfg.dayPct[ch] = (uint8_t)clampf((float)pct, 0.0f, 100.0f);
        settingsMarkDirty();
      }
    }
  }
  if (httpParam(q, "lnight", val, sizeof(val))) {
    char* comma = strchr(val, ',');
    if (comma) {
      *comma = 0;
      long ch = atol(val), pct = atol(comma + 1);
      if (ch >= 0 && ch < LED_CHANNELS) {
        cfg.nightPct[ch] = (uint8_t)clampf((float)pct, 0.0f, 100.0f);
        settingsMarkDirty();
      }
    }
  }
  if (httpParam(q, "on", val, sizeof(val))) {        // on=08:00
    uint8_t h, m;
    if (parseHM(val, h, m)) { cfg.onHour = h; cfg.onMin = m; cfgDirtyFlag = true; }
  }
  if (httpParam(q, "off", val, sizeof(val))) {
    uint8_t h, m;
    if (parseHM(val, h, m)) { cfg.offHour = h; cfg.offMin = m; cfgDirtyFlag = true; }
  }
  if (httpParam(q, "noff", val, sizeof(val))) {      // noff=23:00 — во сколько ночник гаснет
    uint8_t h, m;
    if (parseHM(val, h, m)) { cfg.nightOffHour = h; cfg.nightOffMin = m; cfgDirtyFlag = true; }
  }
  if (httpParam(q, "ramp", val, sizeof(val))) {
    cfg.rampMin = (uint8_t)clampf((float)httpInt(q, "ramp", cfg.rampMin), 1, 180);
    settingsMarkDirty();
  }
  if (httpParam(q, "days", val, sizeof(val))) {
    cfg.daysMask = (uint8_t)(httpInt(q, "days", cfg.daysMask) & 0x7F);
    settingsMarkDirty();
  }

  /* ---------- датчики ---------- */
  if (httpParam(q, "scan", val, sizeof(val)) && atoi(val)) {
    sensorMap();
  }
  if (httpParam(q, "water", val, sizeof(val))) {
    long idx = atol(val);
    DeviceAddress a;
    if (ds.getAddress(a, (uint8_t)idx)) {
      memcpy(cfg.waterRom, a, 8);
      settingsMarkDirty();
      sensorMap();
    } else { ok = false; err = "датчик не найден"; }
  }

  /* ---------- время и сброс ---------- */
  if (httpParam(q, "timesync", val, sizeof(val)) && atoi(val)) {
    if (!ntpSync()) { ok = false; err = "NTP недоступен"; }
  }
  if (httpParam(q, "conf", val, sizeof(val)) && atoi(val)) {
    settingsSaveNow();
  }
  if (httpParam(q, "reset", val, sizeof(val)) && atoi(val)) {
    settingsDefaults();
    settingsSaveNow();
    sensorMap();
  }
  if (httpParam(q, "reboot", val, sizeof(val)) && atoi(val)) {
    snprintf(reply, replyLen, "{\"ok\":1,\"reboot\":1}");
    return;
  }

  snprintf(reply, replyLen, "{\"ok\":%d,\"err\":\"%s\"}", ok ? 1 : 0, err);
}

static void handleRequest(WiFiClient& c, const char* method, char* path, const char* query, char* body) {
  httpRequests++;
  if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
    sendPage(c);
    IPAddress cl = c.remoteIP();
    Serial.print(F("[http] открыт интерфейс, клиент "));
    Serial.print(cl[0]); Serial.print('.'); Serial.print(cl[1]); Serial.print('.');
    Serial.print(cl[2]); Serial.print('.'); Serial.println(cl[3]);
  } else if (strcmp(path, "/api/state") == 0) {
    sendState(c);
  } else if (strcmp(path, "/api/history") == 0) {
    sendHistory(c);
  } else if (strcmp(path, "/api/scan") == 0) {
    sendScan(c);
  } else if (strcmp(path, "/api/cmd") == 0) {
    static char q[512];
    q[0] = 0;
    if (query && *query) { strncpy(q, query, sizeof(q) - 1); q[sizeof(q) - 1] = 0; }
    if (body && *body) {
      size_t l = strlen(q);
      if (l < sizeof(q) - 2) { q[l] = '&'; strncpy(q + l + 1, body, sizeof(q) - l - 2); q[sizeof(q) - 1] = 0; }
    }
    char reply[160];
    handleCmd(q, reply, sizeof(reply));
    httpCmds++;
#if LOG_HTTP_CMDS
    Serial.print(F("[http] команда: "));
    Serial.print(q);
    Serial.print(F("  →  "));
    Serial.println(reply);
#endif
    jsonHead(c, "200 OK");
    c.print(reply);
    c.flush();
    /* отложенная перезагрузка */
    if (strstr(reply, "\"reboot\":1")) { delay(300); NVIC_SystemReset(); }
  } else if (strcmp(path, "/favicon.ico") == 0) {
    c.print(F("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"));
  } else {
    Serial.print(F("[http] неизвестный запрос: "));
    Serial.print(method);
    Serial.print(' ');
    Serial.println(path);
    jsonHead(c, "404 Not Found");
    c.print(F("{\"ok\":0,\"err\":\"not found\"}"));
    c.flush();
  }
}

static void httpTask() {
  WiFiClient c = server.available();
  if (!c) return;

  char line[220];
  if (!readLine(c, line, sizeof(line))) { c.stop(); return; }
  if (line[0] == 0) { c.stop(); return; }

  char method[8] = { 0 };
  char path[96] = { 0 };
  char query[400] = { 0 };
  char* sp1 = strchr(line, ' ');
  if (!sp1) { c.stop(); return; }
  *sp1 = 0;
  strncpy(method, line, sizeof(method) - 1);
  char* target = sp1 + 1;
  char* sp2 = strchr(target, ' ');
  if (sp2) *sp2 = 0;
  char* qm = strchr(target, '?');
  if (qm) {
    *qm = 0;
    strncpy(query, qm + 1, sizeof(query) - 1);
  }
  strncpy(path, target, sizeof(path) - 1);

  /* заголовки: нужен только Content-Length */
  long clen = 0;
  for (uint8_t i = 0; i < 24; i++) {
    if (!readLine(c, line, sizeof(line))) break;
    if (line[0] == 0) break;
    if (strncasecmp(line, "Content-Length:", 15) == 0) clen = strtol(line + 15, NULL, 10);
  }

  static char body[256];
  body[0] = 0;
  if (clen > 0) {
    if (clen > (long)sizeof(body) - 1) clen = (long)sizeof(body) - 1;
    uint32_t t0 = millis();
    long got = 0;
    while (got < clen && (millis() - t0) < HTTP_TIMEOUT_MS) {
      if (c.available()) body[got++] = (char)c.read();
      else if (!c.connected()) break;
      else delay(1);
    }
    body[got] = 0;
  }

  handleRequest(c, method, path, query, body);
  delay(5);
  c.stop();
}

/* ====================== ИНДИКАЦИЯ ====================== */
static void statusLedTask() {
#if STATUS_LED_ENABLE
  static uint32_t t = 0;
  static bool on = false;
  uint32_t now = millis();
  uint32_t period;
  if (alarms & (AL_WATER_SENSOR | AL_OVERHEAT | AL_HEAT_TIMEOUT)) period = 200;   // авария
  else if (!wifiWasConnected && !apMode) period = 500;                            // нет сети
  else if (apMode) period = 1600;                                                 // режим точки доступа
  else period = 1000;                                                             // норма

  if (now - t >= period) {
    t = now;
    on = !on;
    digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
  }
#endif
}

/* ====================== ОТЛАДОЧНЫЙ CLI ====================== */
static void cliHelp() {
  Serial.println(F("--- команды в мониторе порта ---"));
  Serial.println(F("  s — состояние: температуры, реле, свет, тревоги"));
  Serial.println(F("  i — сеть: адрес, шлюз, RSSI, режим"));
  Serial.println(F("  a — поднять свою точку доступа (если адрес потерялся)"));
  Serial.println(F("  w — переподключиться к роутеру"));
  Serial.println(F("  d — список датчиков DS18B20 с адресами"));
  Serial.println(F("  x — тест реле 1..3 (нагреватель не трогаем)"));
  Serial.println(F("  n — синхронизировать время по NTP"));
  Serial.println(F("  l — режим света: авто → ночник → ручной"));
  Serial.println(F("  p — помпа,  o — компрессор,  v — вентилятор вкл/выкл"));
  Serial.println(F("  c — сохранить настройки,  r — перезагрузка,  h — справка"));
}

static void cliTask() {
#if SERIAL_CLI_ENABLE
  if (!Serial.available()) return;
  char cmd = (char)Serial.read();
  while (Serial.available()) Serial.read();
  char b[40];
  switch (cmd) {
    case 's': {
      Serial.println();
      Serial.print(F("  вода      : ")); fmtF(b, sizeof(b), waterOk ? waterTemp : NAN, 2);
      Serial.print(b); Serial.println(F(" °C"));
      Serial.print(F("  воздух    : ")); fmtF(b, sizeof(b), airOk ? airTemp : NAN, 2);
      Serial.print(b); Serial.println(F(" °C"));
      Serial.print(F("  уставка   : ")); fmtF(b, sizeof(b), cfg.setpoint, 1);
      Serial.print(b); Serial.print(F(" ± ")); fmtF(b, sizeof(b), cfg.hyst, 1);
      Serial.print(b); Serial.println(F(" °C"));
      Serial.print(F("  реле      : помпа ")); Serial.print(relayState[RLY_PUMP] ? F("ВКЛ") : F("выкл"));
      Serial.print(F(", компрессор "));       Serial.print(relayState[RLY_AIR] ? F("ВКЛ") : F("выкл"));
      Serial.print(F(", вентилятор "));       Serial.print(relayState[RLY_FAN] ? F("ВКЛ") : F("выкл"));
      Serial.print(F(", нагрев "));            Serial.println(relayState[RLY_HEAT] ? F("ВКЛ") : F("выкл"));
      Serial.print(F("  свет      : ")); Serial.print((unsigned)ledPct[0]); Serial.print(F("% ("));
      Serial.print(PHASE_NAMES[(int)lightPhase]); Serial.print(F("), режим "));
      Serial.println(cfg.lightMode == 0 ? F("авто") : cfg.lightMode == 2 ? F("ночник") : F("ручной"));
      Serial.print(F("  тревоги   : "));
      Serial.println(alarms ? alarmText : "нет");
      break;
    }
    case 'i': {
      Serial.println();
      Serial.print(F("  режим     : "));
      Serial.println(apMode ? F("точка доступа") : F("клиент роутера"));
      Serial.print(F("  адрес     : http://")); Serial.println(ipStr);
      Serial.print(F("  шлюз      : ")); Serial.println(gwStr);
      Serial.print(F("  RSSI      : ")); Serial.print((long)rssi); Serial.println(F(" dBm"));
      Serial.print(F("  статус    : ")); Serial.println(wlStatusName(WiFi.status()));
      Serial.print(F("  запросов  : ")); Serial.print((unsigned long)httpRequests);
      Serial.print(F(", команд ")); Serial.println((unsigned long)httpCmds);
      Serial.print(F("  настройки : "));
      Serial.println(settingsFromNvm ? F("из флеш-памяти") : F("по умолчанию"));
      break;
    }
    case 'a':
      startAP();
      break;
    case 'w':
      Serial.println(F("[wifi] переподключаюсь к роутеру"));
      wasInAp = false;
      apMode = false;
      bannerValid = false;
      wifiNextTry = 0;
      connectSta();
      break;
    case 'd':
      sensorMap();
      printSensorList();
      break;
    case 'x': {
      Serial.println(F("[тест] по очереди включаю реле 1..3 (нагреватель не трогаю)"));
      bool s1 = relayState[RLY_PUMP], s2 = relayState[RLY_AIR], s3 = relayState[RLY_FAN];
      for (uint8_t ch = 0; ch < 3; ch++) {
        Serial.print(F("  канал ")); Serial.println((unsigned)(ch + 1));
        relayWrite(ch, true);  delay(1200); wdtFeed();
        relayWrite(ch, false); delay(600);  wdtFeed();
      }
      relayWrite(RLY_PUMP, s1); relayWrite(RLY_AIR, s2); relayWrite(RLY_FAN, s3);
      Serial.println(F("[тест] готово, прежние состояния восстановлены"));
      break;
    }
    case 'n':
      Serial.println(F("[time] синхронизация по NTP..."));
      Serial.println(ntpSync() ? F("[time] готово") : F("[time] не удалось, проверьте интернет у роутера"));
      break;
    case 'l':
      cfg.lightMode = (uint8_t)((cfg.lightMode + 1) % 3);
      settingsMarkDirty();
      Serial.println(cfg.lightMode == 0 ? F("свет: авто (рассвет-день-закат-ночник)")
                     : cfg.lightMode == 2 ? F("свет: ночник")
                                          : F("свет: ручной"));
      break;
    case 'p': relayWrite(RLY_PUMP, !relayState[RLY_PUMP]); cfg.pumpOn = relayState[RLY_PUMP]; cfgDirtyFlag = true; break;
    case 'o': relayWrite(RLY_AIR, !relayState[RLY_AIR]);   cfg.airOn = relayState[RLY_AIR];   cfgDirtyFlag = true; break;
    case 'v': fanManual = true; fanManualState = !relayState[RLY_FAN]; relayWrite(RLY_FAN, fanManualState); break;
    case 'c':
      settingsSaveNow();
      Serial.println(F("[nvm] настройки сохранены"));
      break;
    case 'r':
      settingsSaveNow();
      Serial.println(F("[sys] настройки сохранены, перезагрузка через мгновение"));
      delay(150);
      NVIC_SystemReset();
      break;
    case 'h':
    default:
      cliHelp();
      break;
  }
#endif
}

/* ====================== SETUP / LOOP ====================== */
void setup() {
  /* 1. Сначала — безопасное состояние реле: до инициализации Wi-Fi ничего не щёлкает.
     При RELAY_ACTIVE_LOW низкий уровень = ВКЛЮЧЕНО, поэтому сразу ставим HIGH. */
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
#if RELAY_ACTIVE_LOW
    digitalWrite(RELAY_PINS[i], HIGH);      // ВЫКЛЮЧЕНО
#else
    digitalWrite(RELAY_PINS[i], LOW);       // ВЫКЛЮЧЕНО
#endif
  }
  relayState[RLY_PUMP] = relayState[RLY_AIR] = relayState[RLY_FAN] = relayState[RLY_HEAT] = false;

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  for (uint8_t i = 0; i < LED_CHANNELS; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    analogWrite(LED_PINS[i], 0);
  }
#if FAN_PWM_ENABLE
  pinMode(PIN_FAN_PWM, OUTPUT);
  analogWrite(PIN_FAN_PWM, 0);
#endif

  /* гамма-таблица для плавного диммирования */
  for (uint8_t i = 0; i <= 100; i++) {
    float v = powf((float)i / 100.0f, LED_PWM_GAMMA) * 255.0f + 0.5f;
    gammaLut[i] = (uint8_t)clampf(v, 0.0f, 255.0f);
  }

  /* 2. Порт: ждём монитор недолго, чтобы плата запускалась и без него */
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) { }
  delay(100);

  resetCauseBits = readResetCause();
  Serial.println();
  Serial.println(F("============================================================"));
  Serial.print(F("   "));
  Serial.print(F(FW_NAME));
  Serial.print(F("  v"));
  Serial.print(F(FW_VERSION));
  Serial.println(F("   ·   Arduino MKR WiFi 1010"));
  Serial.print(F("   причина перезагрузки: "));
  Serial.println(resetCauseName(resetCauseBits));
  Serial.println(F("============================================================"));

  /* 3. Настройки из флеш-памяти */
  bool loaded = settingsLoad();
  Serial.println(loaded ? F("[nvm] настройки загружены из флеш-памяти")
                        : F("[nvm] настройки по умолчанию (первый запуск или другая версия)"));

  /* 4. Датчики */
  ds.begin();
  ds.setWaitForConversion(false);
  ds.setResolution(12);
  sensorMap();
  printSensorList();

  /* 5. Часы */
  timeBegin();
  {
    DateTime dt; epochToLocal(nowEpoch(), dt);
    char b[64];
    snprintf(b, sizeof(b), "[time] часы: %02u.%02u.%04u %02u:%02u (до связи с NTP — время компиляции)",
             dt.day, dt.month, dt.year, dt.hour, dt.minute);
    Serial.println(b);
  }

  /* 6. Реле в состояния, сохранённые в настройках */
  relayWrite(RLY_PUMP, cfg.pumpOn != 0);
  relayWrite(RLY_AIR, cfg.airOn != 0);
  heaterOffAt = millis() - MIN_HEATER_OFF_MS;

  Serial.println();
  printConfigSummary();

  /* 7. Сеть */
  ntpUdp.begin(2390);
  WiFi.setTimeout(3000);        // не ждать «зависший» сокет бесконечно
  server.begin();
  Serial.println(F("[http] веб-сервер запущен (порт 80), ждём подключения к сети"));
  connectSta();
  wifiNextTry = millis() + WIFI_RETRY_MS;

  /* 8. Сторожевой таймер и подсказка */
  lastHeatTick = millis();
  wdtBegin();
  Serial.println();
  Serial.println(F("[подсказка] h — список команд, s — состояние, a — поднять точку доступа"));
}

void loop() {
  wdtFeed();

  /* Монитор порта открыли позже — печатаем сводку заново: адрес, настройки, датчики */
  bool serialOpen = (bool)Serial;
  if (serialOpen && !serialWasOpen) {
    serialWasOpen = true;
    Serial.println();
    Serial.println(F("[монитор] порт открыт — печатаю текущее состояние"));
    printConfigSummary();
    if (bannerValid) printNetBanner(apMode ? "режим точки доступа" : "подключено к роутеру");
    else             Serial.println(F("  адрес пока не получен — плата подключается к сети...\n"));
    printSensorList();
    cliHelp();
  } else if (!serialOpen) {
    serialWasOpen = false;
  }

  wifiTask();
  httpTask();
  sensorTask();
  heaterTask();
  fanTask();
  lightTask();
  timeTask();
  settingsTask();
  statusLedTask();
  buildAlarmText();
  logAlarms();
  cliTask();

  uint32_t now = millis();
  if (histCount == 0 || (now - histAt) >= HISTORY_PERIOD_MS) {
    histAt = now;
    historyPush();
  }

  /* счётчик наработки нагревателя */
  uint32_t dt = now - lastHeatTick;
  lastHeatTick = now;
  if (relayState[RLY_HEAT]) {
    heatMsTotal += dt;
    heatMsToday += dt;
  }

  /* предупреждение о времени: если NTP так и не ответил */
  if (!timeFromNtp && millis() > 120000UL) alarms |= AL_NO_TIME;
  else if (timeFromNtp) alarms &= (uint8_t)~AL_NO_TIME;

  /* периодическая сводка в монитор порта */
  if (now - lastHeartbeat >= LOG_HEARTBEAT_MS) {
    lastHeartbeat = now;
    printHeartbeat();
  }
}
