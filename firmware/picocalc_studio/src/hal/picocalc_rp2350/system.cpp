// system.cpp - boot order, timing, memory, SD card, crash breadcrumbs and the PicoMultiLoader
// hand-off on the RP2350 (Pico 2 W, Hazard3 RISC-V cores, arduino-pico in FreeRTOS mode).
#ifdef ARDUINO_ARCH_RP2040
#include "../../../board.h"
#include <Arduino.h>
#include <SPI.h>
#include <SDFS.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "pico/rand.h"
#include "hardware/uart.h"
#include "../hal.h"

bool rpWaitForMainboard();         // input.cpp
bool rpDisplayInit();              // display.cpp
void rpInputInit();                // input.cpp
void rpAudioInit();                // audio.cpp
void rpPosixInit();                // posix.cpp
void rpClockInit();                // display.cpp

// PicoMultiLoader's boot stub opens its menu once when it finds these in the watchdog scratch
// registers (which survive a watchdog reboot).
#define LOADER_MAGIC 0xE98CC638u

static bool s_sdOk = false;
static SemaphoreHandle_t s_logLock = nullptr;

uint32_t hal_millis()          { return millis(); }
uint64_t hal_micros()          { return time_us_64(); }
void     hal_delay(uint32_t ms){ delay(ms); }
uint32_t hal_random()          { return get_rand_32(); }

extern "C" size_t rp_free_heap(void) { return (size_t)rp2040.getFreeHeap(); }

// Console: the PicoCalc's USB-C (UART0 through the mainboard's CH340) and the Pico's own USB.
static void logWrite(const char *s) {
  if (s_logLock) xSemaphoreTake(s_logLock, portMAX_DELAY);
  Serial1.println(s);
  if (Serial) Serial.println(s);
  if (s_logLock) xSemaphoreGive(s_logLock);
}

void hal_log(const char *fmt, ...) {
  char b[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  logWrite(b);
}

// Our own reboots say so, so the next boot doesn't report them as a crash.
#define REBOOT_MAGIC 0x8EB0075Du
static uint32_t __uninitialized_ram(s_rebootMagic);

[[noreturn]] static void watchdogReboot() {
  watchdog_reboot(0, 0, 1);
  for (;;) tight_loop_contents();
}

void hal_reboot() {
  delay(50);
  s_rebootMagic = REBOOT_MAGIC;
  watchdogReboot();
}

void hal_reboot_to_loader() {
  hal_log("rebooting to PicoMultiLoader");
  delay(50);
  s_rebootMagic = REBOOT_MAGIC;
  watchdog_hw->scratch[0] = LOADER_MAGIC;   // without the loader: plain restart
  watchdog_hw->scratch[1] = 1;
  watchdogReboot();
}

// Uninitialized RAM keeps its contents through a watchdog reboot, not through power-off; the
// magics tell a real record from power-on garbage.
#define CRUMB_MAGIC 0xC0FFEE5Du
#define PANIC_MAGIC 0xDEAD005Du
#define PANIC_BT    12
static uint32_t __uninitialized_ram(s_crumbMagic);
static char     __uninitialized_ram(s_crumb)[96];
static uint32_t __uninitialized_ram(s_panicMagic);
static uint32_t __uninitialized_ram(s_panicPc);
static uint32_t __uninitialized_ram(s_panicRa);
static uint32_t __uninitialized_ram(s_panicBt)[PANIC_BT];
static char     __uninitialized_ram(s_panicWhy)[48];

// Flash (XIP) and SRAM, where compiled programs run.
static bool isCode(uint32_t v) {
  return (v >= 0x10000000u && v < 0x10400000u) || (v >= 0x20000000u && v < 0x20082000u);
}

static void setWhy(const char *why) {
  size_t i = 0;
  for (; i < sizeof(s_panicWhy) - 1 && why[i]; i++) s_panicWhy[i] = why[i];
  s_panicWhy[i] = 0;
}

// The crash record on the console too, straight to UART0 (no locks, no tasks): the boot path
// through PicoMultiLoader's stub can overwrite low RAM before the next boot reads the record.
static void crashPrint() {
  char b[200];
  int len = snprintf(b, sizeof b, "\r\ncrash: %s, pc %08lx, ra %08lx, stack", s_panicWhy, (unsigned long)s_panicPc,
                     (unsigned long)s_panicRa);
  for (int i = 0; i < PANIC_BT && s_panicBt[i] && len < (int)sizeof b - 12; i++)
    len += snprintf(b + len, sizeof b - len, " %08lx", (unsigned long)s_panicBt[i]);
  snprintf(b + len, sizeof b - len, "\r\n");
  for (const char *p = b; *p; p++) uart_putc_raw(uart0, *p);
  uart_tx_wait_blocking(uart0);
}

static const char *causeName(uint32_t mcause) {
  switch (mcause) {
    case 0: return "instruction misaligned";
    case 1: return "instruction access fault";
    case 2: return "illegal instruction";
    case 3: return "breakpoint";
    case 4: return "load misaligned";
    case 5: return "load access fault";
    case 6: return "store misaligned";
    case 7: return "store access fault";
    default: return "exception";
  }
}

// FreeRTOS's trap handler saves the faulting task's registers on its stack ([0] pc, [1] ra,
// x5..x31, 31 words) and the stack pointer in the TCB's first word, then calls this on the ISR
// stack for anything but an ecall. Record the crash and reboot: the next boot reports it.
extern "C" void freertos_risc_v_application_exception_handler(uint32_t mcause) {
  uint32_t mepc;
  __asm__ volatile("csrr %0, mepc" : "=r"(mepc));
  s_panicPc = mepc;
  s_panicRa = 0;
  memset(s_panicBt, 0, sizeof(s_panicBt));
  TaskHandle_t t = xTaskGetCurrentTaskHandleForCore((BaseType_t)get_core_num());
  const uint32_t *frame = t ? *(const uint32_t *const *)t : nullptr;
  if (frame && ((uintptr_t)frame & 3) == 0 && (uintptr_t)frame >= 0x20000000u && (uintptr_t)frame < 0x20082000u - 31 * 4) {
    if (isCode(frame[1])) s_panicRa = frame[1];
    const uint32_t *sp = frame + 31;
    int n = 0;
    for (int i = 0; i < 256 && n < PANIC_BT && (uintptr_t)(sp + i) < 0x20082000u; i++)
      if (isCode(sp[i]) && (sp[i] & 1) == 0 && sp[i] >= 0x10000000u + 0x100) s_panicBt[n++] = sp[i];
  }
  setWhy(causeName(mcause));
  s_panicMagic = PANIC_MAGIC;
  crashPrint();
  watchdogReboot();
}

extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *name) {
  s_panicPc = s_panicRa = 0;
  memset(s_panicBt, 0, sizeof(s_panicBt));
  char b[48];
  snprintf(b, sizeof b, "stack overflow in task %s", name ? name : "?");
  setWhy(b);
  s_panicMagic = PANIC_MAGIC;
  crashPrint();
  watchdogReboot();
}

// The core's default panics; a failed xTaskCreate already returns an error to the caller.
extern "C" void vApplicationMallocFailedHook(void) {}

void hal_breadcrumb(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_crumb, sizeof(s_crumb), fmt, ap);
  va_end(ap);
  s_crumbMagic = CRUMB_MAGIC;
}

static bool s_bootChecked = false, s_bootCrashed = false, s_bootPanic = false;

// Decided once at boot: later reboots of ours set the magic again.
static void checkBoot() {
  if (s_bootChecked) return;
  s_bootChecked = true;
  const bool ours = s_rebootMagic == REBOOT_MAGIC;
  s_rebootMagic = 0;
  s_bootPanic = s_panicMagic == PANIC_MAGIC;
  s_bootCrashed = s_bootPanic || (watchdog_caused_reboot() && !ours && watchdog_enable_caused_reboot());
}

bool hal_crash_report(char *buf, size_t n) {
  checkBoot();
  const bool crumb = s_crumbMagic == CRUMB_MAGIC, panic = s_panicMagic == PANIC_MAGIC;
  s_crumbMagic = s_panicMagic = 0;
  if (!s_bootCrashed) return false;
  s_bootCrashed = false;
  s_crumb[sizeof(s_crumb) - 1] = 0;
  int len = snprintf(buf, n, "%s, during: %s.", panic ? "crash (exception)" : "watchdog", crumb ? s_crumb : "?");
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
  checkBoot();
  if (s_panicMagic != PANIC_MAGIC || !s_bootPanic) return 0;
  int n = 0;
  if (s_panicPc && n < max) pcs[n++] = s_panicPc;
  if (s_panicRa && n < max) pcs[n++] = s_panicRa;
  for (int i = 0; i < PANIC_BT && s_panicBt[i] && n < max; i++) pcs[n++] = s_panicBt[i];
  return n;
}

// Commands from the PC over either serial port, one per line:
//   @loader        reboot into PicoMultiLoader (@uf2menu also works)
//   @reboot        plain restart
//   @key <hex>     press and release a key (picocalc.h KEY_* codes), e.g. @key b1 for Esc
// Anything else starting with @ goes to the hook (the runtime's @test).
static bool (*s_cmdHook)(const char *line);
void hal_set_serial_cmd_hook(bool (*hook)(const char *line)) { s_cmdHook = hook; }

struct LineBuf { char line[96]; uint8_t len; };

static void serialCmd(Stream &s, LineBuf &b) {
  while (s.available()) {
    const int c = s.read();
    if (c < 0) break;
    if (c != '\n' && c != '\r') { if (b.len < sizeof(b.line) - 1) b.line[b.len++] = (char)c; continue; }
    b.line[b.len] = 0;
    const char *line = b.line;
    b.len = 0;
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

void rpCheckSerialCmd() {
  static LineBuf uart, usb;
  serialCmd(Serial1, uart);
  serialCmd(Serial, usb);
}

// No PSRAM: everything comes from the one SRAM heap.
void  *hal_alloc_big(size_t n)  { return calloc(1, n); }
void  *hal_alloc_fast(size_t n) { return calloc(1, n); }
void   hal_free(void *p)        { free(p); }
size_t hal_free_heap()          { return rp_free_heap(); }

bool        hal_sd_mounted() { return s_sdOk; }
const char *hal_sd_root()    { return SD_MOUNT; }

static void sdInit() {
  SPI.setRX(SD_MISO_PIN);
  SPI.setTX(SD_MOSI_PIN);
  SPI.setSCK(SD_SCK_PIN);
  SDFSConfig cfg(SD_CS_PIN, SD_SPI_HZ, SPI);
  SDFS.setConfig(cfg);
  for (int i = 0; i < 10; i++) {
    if (SDFS.begin()) { s_sdOk = true; break; }
    SDFS.end();
    delay(120);
  }
  if (s_sdOk) {
    // No SDFS.info() for the size: its usedBytes counts free clusters, reading the whole FAT
    // (8 s on a 32 GB card).
    hal_log("SD: card mounted at %s", SD_MOUNT);
  } else {
    hal_log("SD: mount failed");
  }
}

void hal_init() {
  Serial1.setTX(0);
  Serial1.setRX(1);
  Serial1.begin(115200);
  checkBoot();
  rpClockInit();                     // it changes the clock UART baud rates come from: restart it
  Serial1.end();
  Serial1.begin(115200);
  Serial.begin(115200);
  s_logLock = xSemaphoreCreateMutex();
  hal_log("PicoCalc Studio on " BOARD_NAME " at %lu MHz, %u KB free", (unsigned long)(rp2040.f_cpu() / 1000000),
          (unsigned)(rp_free_heap() >> 10));
  rpPosixInit();
  const bool board = rpWaitForMainboard();
  rpDisplayInit();
  rpAudioInit();
  if (board) sdInit();
  rpInputInit();
}

#endif  // ARDUINO_ARCH_RP2040
