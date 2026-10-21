// system.cpp - boot order, timing, memory, SD card, crash breadcrumbs and the PicoMultiLoader
// hand-off on the P4. From emu16 / pico-386 (system_p4.cpp).
#include "../../../board.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <stdarg.h>
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "soc/soc.h"
#include "soc/lp_system_reg.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "../hal.h"

bool p4WaitForMainboard();         // input.cpp
bool p4DisplayInit();              // display.cpp
void p4InputInit();                // input.cpp
void p4AudioInit();                // audio.cpp

#define LOADER_MAGIC 0x10ADE125u   // PicoMultiLoader main/loader_api.h: boot the loader once

static SPIClass s_sdSpi(HSPI);
static bool s_sdOk = false;

uint32_t hal_millis()          { return millis(); }
uint64_t hal_micros()          { return (uint64_t)esp_timer_get_time(); }
void     hal_delay(uint32_t ms){ delay(ms); }
uint32_t hal_random()          { return esp_random(); }

void hal_log(const char *fmt, ...) {
  char b[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  Serial.println(b);
}

void hal_reboot() { delay(50); esp_restart(); }

void hal_reboot_to_loader() {
  hal_log("rebooting to PicoMultiLoader");
  delay(50);
  REG_WRITE(LP_SYSTEM_REG_LP_STORE15_REG, LOADER_MAGIC);   // without the loader: plain restart
  esp_restart();
}

// Plain (not RTC) noinit RAM keeps its contents through a panic reset, not through power-off;
// the magic tells a real breadcrumb from power-on garbage.
#define CRUMB_MAGIC 0xC0FFEE5Du
#define PANIC_MAGIC 0xDEAD005Du
#define PANIC_BT    12
static __NOINIT_ATTR uint32_t s_crumbMagic;
static __NOINIT_ATTR char     s_crumb[96];
static __NOINIT_ATTR uint32_t s_panicMagic;
static __NOINIT_ATTR uint32_t s_panicPc, s_panicRa, s_panicBt[PANIC_BT];
static __NOINIT_ATTR char     s_panicWhy[48];

static bool isCode(uint32_t v) {
  return (v >= 0x40000000u && v < 0x44000000u) || (v >= 0x4FF00000u && v < 0x4FFC0000u);
}

// The loader's partition table has no core dump partition, so a panic handler keeps the crash PC,
// the reason and the code addresses found on the stack in noinit RAM. The Arduino core calls it
// from its esp_panic_handler wrapper, before the reboot.
// The hook gets no registers, but IDF's panic entry saves them (mepc, ra, sp, ...) just below the
// faulting SP. RA is the only trace of the caller when a call jumped to a bad address. Skipped
// for a stack overflow, where below the SP is the guard region.
static uint32_t savedRa(const arduino_panic_info_t *info) {
  if (!info->backtrace_len || (info->reason && strstr(info->reason, "Stack protection"))) return 0;
  const uint32_t sp = info->backtrace[0];
  if (sp & 15 || sp < 0x4FF00000u + 512 || sp >= 0x4FFC0000u) return 0;
  for (uint32_t a = sp - 16; a >= sp - 512; a -= 16) {
    const uint32_t *f = (const uint32_t *)a;   // [0] mepc, [1] ra, [2] sp
    if (f[2] == sp && f[0] == (uint32_t)info->pc) return isCode(f[1]) ? f[1] : 0;
  }
  return 0;
}

static void onPanic(arduino_panic_info_t *info, void *) {
  s_panicPc = (uint32_t)info->pc;
  s_panicRa = savedRa(info);
  memset(s_panicBt, 0, sizeof(s_panicBt));
  int n = 0;
  for (unsigned i = 1; i < info->backtrace_len && n < PANIC_BT; i++)   // [0] is the SP
    if (isCode(info->backtrace[i])) s_panicBt[n++] = info->backtrace[i];
  const char *why = info->reason ? info->reason : "";
  size_t i = 0;
  for (; i < sizeof(s_panicWhy) - 1 && why[i]; i++) s_panicWhy[i] = why[i];
  s_panicWhy[i] = 0;
  s_panicMagic = PANIC_MAGIC;
}

void hal_breadcrumb(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_crumb, sizeof(s_crumb), fmt, ap);
  va_end(ap);
  s_crumbMagic = CRUMB_MAGIC;
}

bool hal_crash_report(char *buf, size_t n) {
  const esp_reset_reason_t r = esp_reset_reason();
  const char *why = r == ESP_RST_PANIC    ? "crash (exception)"
                  : r == ESP_RST_INT_WDT  ? "interrupt watchdog"
                  : r == ESP_RST_TASK_WDT ? "task watchdog"
                  : r == ESP_RST_WDT      ? "watchdog"
                  : r == ESP_RST_BROWNOUT ? "brownout (power dip)" : nullptr;
  const bool crumb = s_crumbMagic == CRUMB_MAGIC, panic = s_panicMagic == PANIC_MAGIC;
  s_crumbMagic = s_panicMagic = 0;
  if (!why) return false;
  s_crumb[sizeof(s_crumb) - 1] = 0;
  int len = snprintf(buf, n, "%s, during: %s.", why, crumb ? s_crumb : "?");
  if (panic && len < (int)n) {
    s_panicWhy[sizeof(s_panicWhy) - 1] = 0;
    len += snprintf(buf + len, n - len, " %s, pc %08lx, ra %08lx, stack", s_panicWhy, (unsigned long)s_panicPc,
                    (unsigned long)s_panicRa);
    for (int i = 0; i < PANIC_BT && s_panicBt[i] && len < (int)n - 10; i++)
      len += snprintf(buf + len, n - len, " %08lx", (unsigned long)s_panicBt[i]);
  }
  return true;
}

int hal_crash_pcs(uint32_t *pcs, int max) {
  const esp_reset_reason_t r = esp_reset_reason();
  if (s_panicMagic != PANIC_MAGIC || (r != ESP_RST_PANIC && r != ESP_RST_INT_WDT && r != ESP_RST_TASK_WDT &&
                                      r != ESP_RST_WDT)) return 0;
  int n = 0;
  if (n < max) pcs[n++] = s_panicPc;
  if (s_panicRa && n < max) pcs[n++] = s_panicRa;
  for (int i = 0; i < PANIC_BT && s_panicBt[i] && n < max; i++) pcs[n++] = s_panicBt[i];
  return n;
}

// Commands from the PC over the console UART, one per line:
//   @loader        reboot into PicoMultiLoader (@uf2menu also works)
//   @reboot        plain restart
//   @key <hex>     press and release a key (picocalc.h KEY_* codes), e.g. @key b1 for Esc
// Anything else starting with @ goes to the hook (the runtime's @test).
static bool (*s_cmdHook)(const char *line);
void hal_set_serial_cmd_hook(bool (*hook)(const char *line)) { s_cmdHook = hook; }

void p4CheckSerialCmd() {
  static char line[96];
  static uint8_t len = 0;
  while (Serial.available()) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c != '\n' && c != '\r') { if (len < sizeof(line) - 1) line[len++] = (char)c; continue; }
    line[len] = 0;
    len = 0;
    if (!strcmp(line, "@loader") || !strcmp(line, "@uf2menu")) hal_reboot_to_loader();
    else if (!strcmp(line, "@reboot")) hal_reboot();
    else if (!strncmp(line, "@key ", 5)) {
      const uint8_t k = (uint8_t)strtoul(line + 5, nullptr, 16);
      hal_key_inject(k, HAL_KEY_PRESSED);
      vTaskDelay(pdMS_TO_TICKS(60));
      hal_key_inject(k, HAL_KEY_RELEASED);
    }
    else if (line[0] == '@' && !(s_cmdHook && s_cmdHook(line))) hal_log("unknown command %s", line);
  }
}

void *hal_alloc_big(size_t n) {
  void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : calloc(1, n);
}
// "Internal" includes the 32 KB LP SRAM, which is slower than PSRAM through the cache: not that.
static bool isLpRam(const void *p) { return (uintptr_t)p >= 0x50100000u && (uintptr_t)p < 0x50200000u; }
void *hal_alloc_fast(size_t n) {
  void *p = heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (p && isLpRam(p)) { free(p); p = heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT); }
  return p ? p : hal_alloc_big(n);
}
void   hal_free(void *p) { free(p); }
size_t hal_free_heap()   { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }

bool        hal_sd_mounted() { return s_sdOk; }
const char *hal_sd_root()    { return SD_MOUNT; }

static void sdInit() {
  // The SD pins sit in an IO domain fed by an internal LDO that is off at boot; without it
  // GPIO46-48 are dead and every mount fails.
  static esp_ldo_channel_handle_t ldo = nullptr;
  if (!ldo) {
    esp_ldo_channel_config_t c = {};
    c.chan_id = SD_IO_LDO_CHAN;
    c.voltage_mv = 3300;
    if (esp_ldo_acquire_channel(&c, &ldo) != ESP_OK) hal_log("SD: IO LDO enable failed");
  }
  s_sdSpi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  for (int i = 0; i < 10; i++) {
    if (SD.begin(SD_CS_PIN, s_sdSpi, SD_SPI_HZ, SD_MOUNT)) { s_sdOk = true; break; }
    SD.end();
    delay(120);
  }
  if (s_sdOk) hal_log("SD: %lluMB card mounted at %s", SD.cardSize() >> 20, SD_MOUNT);
  else        hal_log("SD: mount failed");
}

void hal_init() {
  set_arduino_panic_handler(onPanic, nullptr);
  Serial.begin(115200);
  delay(50);
  hal_log("PicoCalc Studio on " BOARD_NAME ", PSRAM %u KB free, internal %u KB free",
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10),
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
  const bool board = p4WaitForMainboard();
  p4DisplayInit();
  p4AudioInit();                     // early: it wants DMA-capable SRAM
  if (board) sdInit();
  p4InputInit();
}
