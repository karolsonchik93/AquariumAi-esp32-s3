/* =====================================================================
   nvm.h — хранение настроек во флеш-памяти SAMD21 (MKR WiFi 1010).

   В ядре Arduino SAMD библиотеки EEPROM нет, поэтому здесь реализована
   минимальная запись «в конец флеша» — та же последовательность команд
   NVMCTRL, что и в библиотеке FlashStorage (Cristian Maglie, Arduino LLC).

   Как это работает:
     • берётся последняя строка флеша (256 байт) — она одна, и её адрес
       вычисляется из параметров контроллера (PSZ, NVMP);
     • строка стирается целиком и записывается заново вместе с настройками.

   ВАЖНО: скетч должен быть меньше (размер флеша − 256 байт).
   Наш скетч занимает около 130 КБ из 256 КБ, запас большой.
   ===================================================================== */
#pragma once
#include <Arduino.h>

namespace nvm {

static const uint32_t CMDEX_KEY = 0xA5u << 8;   // NVMCTRL_CTRLA_CMDEX_KEY

inline uint32_t pageSize() {
  static const uint32_t sizes[8] = { 8, 16, 32, 64, 128, 256, 512, 1024 };
  return sizes[NVMCTRL->PARAM.bit.PSZ & 0x07];
}

inline uint32_t rowSize()   { return pageSize() * 4UL; }        // 256 байт
inline uint32_t flashSize() { return pageSize() * (uint32_t)NVMCTRL->PARAM.bit.NVMP; }
inline uint32_t base()      { return flashSize() - rowSize(); } // последняя строка

inline void waitReady() { while (NVMCTRL->INTFLAG.bit.READY == 0) { } }

inline void eraseRow(uint32_t addr) {
  waitReady();
  NVMCTRL->ADDR.reg = addr / 2;                 // в NVMCTRL адрес задаётся в 16-битных словах
  NVMCTRL->CTRLA.reg = CMDEX_KEY | NVMCTRL_CTRLA_CMD_ER;
  waitReady();
}

/* Запись данных (не больше одной строки). Остальные байты строки — 0xFF. */
inline bool write(const void* src, uint16_t len) {
  uint32_t ps = pageSize();
  if (len == 0 || len > rowSize() || ps == 0) return false;

  const uint8_t* s = (const uint8_t*)src;
  uint32_t addr = base();
  eraseRow(addr);

  NVMCTRL->CTRLB.bit.MANW = 1;                  // ручной режим записи страниц

  uint16_t pages = (uint16_t)((len + ps - 1) / ps);
  for (uint16_t p = 0; p < pages; p++) {
    NVMCTRL->CTRLA.reg = CMDEX_KEY | NVMCTRL_CTRLA_CMD_PBC;   // очистить буфер страницы
    waitReady();

    volatile uint32_t* dst = (volatile uint32_t*)(addr + (uint32_t)p * ps);
    for (uint32_t i = 0; i < ps; i += 4) {
      uint32_t off = (uint32_t)p * ps + i;
      uint32_t w = 0xFFFFFFFFu;
      if (off < len) {
        uint8_t tmp[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        for (uint8_t k = 0; k < 4; k++)
          if ((off + k) < len) tmp[k] = s[off + k];
        w = (uint32_t)tmp[0] | ((uint32_t)tmp[1] << 8) |
            ((uint32_t)tmp[2] << 16) | ((uint32_t)tmp[3] << 24);
      }
      *dst = w;
    }
    NVMCTRL->CTRLA.reg = CMDEX_KEY | NVMCTRL_CTRLA_CMD_WP;    // записать страницу
    waitReady();
  }
  return true;
}

inline void read(void* dst, uint16_t len) {
  memcpy(dst, (const void*)base(), len);
}

} // namespace nvm
