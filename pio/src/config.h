/* =====================================================================
   Аквариумный контроллер на ESP32-S3 (модуль N16R8: 16 МБ флеша, 8 МБ PSRAM)
   config.h — все настройки в одном месте.

   Меняйте только этот файл. Прошивка: aquarium_esp32s3.ino
   Схема подключения: ../../WIRING.md
   ===================================================================== */
#pragma once

#define FW_NAME      "Aquarium ESP32-S3"
#define FW_VERSION   "2.4.1"     // 2.4.1 — исправлена кодировка строк в Android-виджете

/* ---------------------------------------------------------------------
   1. Wi-Fi
   --------------------------------------------------------------------- */
/* Значение по умолчанию оставлено как «заглушка»: если его не поменять,
   плата не станет ждать роутер, а сразу поднимет точку доступа
   и напишет об этом в монитор порта. */
#define WIFI_SSID_DEFAULT    "ВАША_СЕТЬ_2.4ГГц"
#define WIFI_SSID            "ВАША_СЕТЬ_2.4ГГц"   // ESP32-S3 работает и в 2.4, и в 5 ГГц
#define WIFI_PASS            "ВАШ_ПАРОЛЬ"

/* Имя платы в сети. Работает настоящий mDNS: http://aquarium.local
   (имя резолвится почти на любом устройстве в сети) */
#define WIFI_HOSTNAME        "aquarium"

/* Своя точка доступа, если роутера нет или связь потерялась */
#define AP_FALLBACK          1
#define AP_SSID              "Aquarium"
#define AP_PASS              "aquarium123"          // не короче 8 символов
#define STA_TIMEOUT_MS       30000UL                // сколько ждать роутер перед переходом в AP
#define WIFI_RETRY_MS        15000UL                // период повторных попыток
#define AP_ON_LOST_MS        (5UL * 60UL * 1000UL)  // нет связи дольше — поднять точку доступа

/* Статический IP: включите, если адрес нужно знать заранее */
#define USE_STATIC_IP        0
#define STATIC_IP            192, 168, 1, 77
#define STATIC_GW            192, 168, 1, 1
#define STATIC_MASK          255, 255, 255, 0
#define STATIC_DNS           192, 168, 1, 1

/* ---------------------------------------------------------------------
   2. Время
   --------------------------------------------------------------------- */
#define TZ_OFFSET_MIN        180                     // Москва = UTC+3
#define NTP_SERVER_1         "pool.ntp.org"
#define NTP_SERVER_2         "time.google.com"
#define NTP_SYNC_PERIOD_MS   (6UL * 3600UL * 1000UL)

/* ---------------------------------------------------------------------
   3. Пины ESP32-S3 (подробности и обоснование — в WIRING.md)
   ---------------------------------------------------------------------
   ВАЖНО: GPIO 26…37 заняты флеш-памятью и PSRAM модуля N16R8 — их трогать
   нельзя. GPIO 19/20 — USB, 43/44 — UART0 (монитор порта),
   GPIO 0/3/45/46 — служебные при загрузке. Всё остальное свободно. */
#define PIN_RELAY_PUMP       4    // канал 1: помпа
#define PIN_RELAY_AIR        5    // канал 2: компрессор / аэратор
#define PIN_RELAY_FAN        6    // канал 3: вентилятор охлаждения
#define PIN_RELAY_HEAT       7    // канал 4: подогреватель 75 Вт

#define PIN_LED_1            12   // ШИМ: основной канал светильника (LEDC)
#define PIN_LED_2            13   // ШИМ: второй канал (если появится)
#define PIN_LED_3            14   // ШИМ: третий канал (если появится)

#define PIN_ONEWIRE          15   // шина 1-Wire: два DS18B20 + резистор 4.7к на 3V3
#define PIN_FAN_PWM          16   // ШИМ вентилятора (только если FAN_PWM_ENABLE 1)
#define PIN_BUZZER           17   // пищалка (необязательно, BUZZER_ENABLE 1)

/* Встроенный адресный светодиод платы (ESP32-S3 DevKitC-1) — им показываем
   состояние: синий «подключаюсь», зелёный «всё хорошо», красный «авария». */
#define STATUS_RGB_ENABLE    1
#define PIN_STATUS_RGB       48

/* Блок реле 5 В с оптопарой — активный НИЗКИЙ уровень (стандарт).
   Преобразователи уровня 3.3→5 В остаются, как на прежней плате. */
#define RELAY_ACTIVE_LOW     1

/* ---------------------------------------------------------------------
   4. Светильник
   --------------------------------------------------------------------- */
#define LED_CHANNELS         1        // 1 = обычный одноцветный светильник
#define LED_PWM_FREQ         5000     // Гц: выше слышимого диапазона, драйверам LDD подходит
#define LED_PWM_BITS         12       // 12 бит = 4096 ступеней яркости
#define LED_PWM_GAMMA        2.2f     // гамма-коррекция для глаза

/* ---------------------------------------------------------------------
   5. Термостат подогревателя
   --------------------------------------------------------------------- */
#define DEF_SETPOINT         25.0f
#define DEF_HYST             0.4f
#define DEF_MAX_WATER_TEMP   30.0f
#define MIN_HEATER_ON_MS     (20UL * 1000UL)
#define MIN_HEATER_OFF_MS    (90UL * 1000UL)
#define MAX_HEATER_ON_MS     (45UL * 60UL * 1000UL)
#define HEATER_LOCK_MS       (10UL * 60UL * 1000UL)
#define DEF_HEATER_POWER_W   75.0f

/* ---------------------------------------------------------------------
   6. Вентилятор охлаждения
   --------------------------------------------------------------------- */
#define DEF_FAN_ON_TEMP      27.0f
#define DEF_FAN_HYST         0.5f
#define FAN_PWM_ENABLE       0        // 1 = плавная скорость вентилятора через ШИМ

/* ---------------------------------------------------------------------
   7. Светильник: расписание по умолчанию
   --------------------------------------------------------------------- */
#define DEF_LIGHT_ON_H       8
#define DEF_LIGHT_ON_M       0
#define DEF_LIGHT_OFF_H      20
#define DEF_LIGHT_OFF_M      0
#define DEF_LIGHT_RAMP_MIN   40
#define DEF_DAY_PCT_1        80
#define DEF_DAY_PCT_2        60
#define DEF_DAY_PCT_3        0
#define DEF_NIGHT_PCT_1      5        // ночник: слабый свет после заката
#define DEF_NIGHT_PCT_2      0
#define DEF_NIGHT_PCT_3      0
#define DEF_NIGHT_OFF_H      23       // во сколько ночник гаснет совсем
#define DEF_NIGHT_OFF_M      0
#define DEF_DAYS_MASK        0x7F

/* ---------------------------------------------------------------------
   8. Что умеет новая плата (включается здесь)
   --------------------------------------------------------------------- */
#define ENABLE_OTA           1        // обновление прошивки по сети и через сайт
#define ENABLE_MDNS          1        // http://aquarium.local
#define ENABLE_SSE           1        // живая телеметрия без опроса (Server-Sent Events)
#define ENABLE_WEB_OTA       1        // загрузка .bin прямо со страницы

/* ---------------------------------------------------------------------
   Обновление по кнопке из GitHub.
   Плата скачивает этот файл и прошивается им — перепрошивать по проводу
   больше не нужно. Свой .bin положите в репозиторий (папка build/prebuilt/)
   и подставьте ссылку вида:
     https://raw.githubusercontent.com/<логин>/<репозиторий>/main/build/prebuilt/aquarium_esp32s3.bin
   --------------------------------------------------------------------- */
#define ENABLE_GITHUB_OTA    1
#define OTA_GITHUB_URL       "https://raw.githubusercontent.com/karolsonchik93/AquariumAi-esp32-s3/main/build/prebuilt/aquarium_esp32s3.bin"

/* Файл с номером версии — по нему страница показывает, что именно нашлось на GitHub.
   В репозитории это build/prebuilt/version.txt с содержимым вида «2.3.0». */
#define ENABLE_GITHUB_VERSION 1
#define OTA_GITHUB_VERSION_URL "https://raw.githubusercontent.com/karolsonchik93/AquariumAi-esp32-s3/main/build/prebuilt/version.txt"

#define HISTORY_HOURS        24       // глубина истории, часов (хранится в PSRAM)
#define HISTORY_PERIOD_S     15       // шаг записи, секунд
#define HISTORY_MAX_POINTS   ((HISTORY_HOURS) * 3600 / (HISTORY_PERIOD_S))

#define SENSOR_PERIOD_MS     2000UL
#define SENSOR_CONV_MS       800UL
#define SENSOR_FAIL_COUNT    3
#define TEMP_MIN_VALID       (-20.0f)
#define TEMP_MAX_VALID       (80.0f)

#define HTTP_PORT            80
#define HTTP_TIMEOUT_MS      4000UL
#define CYCLE_LOG_SIZE       12
#define NVM_NAMESPACE        "aqua"

#define WATCHDOG_ENABLE      1        // сторожевой таймер: зависшую прошивку перезагружаем
#define WATCHDOG_TIMEOUT_S   30
#define STATUS_LED_ENABLE    1        // мигать встроенным светодиодом
#define SERIAL_CLI_ENABLE    1        // отладочные команды через Serial (115200)
#define LOG_HEARTBEAT_MS     60000UL  // сводка состояния в монитор порта
#define LOG_HTTP_CMDS        1

/* Массивы каналов света: используются первые LED_CHANNELS элементов */
const uint8_t LED_PINS[3] = { PIN_LED_1, PIN_LED_2, PIN_LED_3 };
const char* const LED_LABELS[3] = { "Светильник", "Канал 2", "Канал 3" };
