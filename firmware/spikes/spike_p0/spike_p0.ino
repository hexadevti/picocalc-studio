// spike_p0 - Phase 0 feasibility checks for PicoCalc Studio (ESP32-P4, Arduino-ESP32 3.3.x).
//
// (a) Run compiled code from flash: write a routine into the free tail of the running ota_0
//     partition, mmap it as instructions and call it. The routine calls back into the firmware
//     and reads a constant next to itself. Then rewrite it with a different version and call
//     again, so a stale I-cache shows up as a wrong result.
//     IDF refuses any write inside the running partition (CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS
//     aborts), so the default flash chip's region_protected hook is wrapped to allow exactly
//     [end of image, end of partition) and defer to IDF for everything else.
// (b) Export slot: a 1 MB 0xFF block in .flash.text (pc_slot). tools/host/esp_image.py patches a
//     probe into it and fixes the image checksum + SHA-256. Flash the patched .bin through
//     PicoMultiLoader: this sketch then reports "slot: PATCHED" and calls into the slot.
//
// Results go to the LCD and the serial port, and each line is appended to /sd/picocalc_p0.txt as it
// is logged. After a crash the app restarts, reports the step that died and skips (a).
// Build: tools/build.ps1 firmware\spikes\spike_p0 -ProbeSlot (writes only ota_0-compatible app images; never upload with
// arduino-cli, which would overwrite the PicoMultiLoader bootloader and partition table).

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <stdarg.h>
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_flash.h"
#include "esp_ldo_regulator.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "hal/usb_serial_jtag_ll.h"
#include "freertos/semphr.h"
#include "uifont.h"

// ---------------------------------------------------------------------------------------------
// Export slot (b). Lives in .flash.text via the "*(.text.*)" input pattern, so it is mapped RX.

asm(".section .text.pc_slot,\"ax\",@progbits\n"
    ".balign 4\n"   // more than the default .flash.text alignment breaks IDF's dummy rodata section
    ".globl pc_slot\n"
    "pc_slot:\n"
    ".fill 0x100000, 1, 0xff\n"
    ".previous\n");
extern "C" const uint8_t pc_slot[];

#define SLOT_MAGIC 0x4C534350u   // "PCSL"
struct SlotHeader {              // must match tools/host/esp_image.py make-probe-slot
  uint32_t magic;
  uint32_t entry;                // offset of the entry point from pc_slot
  uint32_t flags;                // bit 0: autorun
  uint32_t reserved;
};

// ---------------------------------------------------------------------------------------------
// Probe routine (a), assembled from firmware/spikes/probe/probe.S and linked at 0.
// int probe(int a, int (*cb)(int), int k) = cb(a) + k + IMM + DATA, v1: IMM 1 DATA 1000,
// v2: IMM 7 DATA 2000. Position independent (auipc + lw), no compressed instructions.

static const uint32_t PROBE_V1[] = {
  0xff010113, 0x00112623, 0x00812423, 0x00060413, 0x000580e7,
  0x00850533, 0x00150513, 0x00000297, 0x01c2a303, 0x00650533,
  0x00812403, 0x00c12083, 0x01010113, 0x00008067, 0x000003e8,
};
static const uint32_t PROBE_V2[] = {
  0xff010113, 0x00112623, 0x00812423, 0x00060413, 0x000580e7,
  0x00850533, 0x00750513, 0x00000297, 0x01c2a303, 0x00650533,
  0x00812403, 0x00c12083, 0x01010113, 0x00008067, 0x000007d0,
};
typedef int (*probe_fn)(int, int (*)(int), int);

static volatile int s_cbCalls;
static int cbTriple(int x) { s_cbCalls++; return x * 3; }
static int expectV1(int a, int k) { return a * 3 + k + 1 + 1000; }
static int expectV2(int a, int k) { return a * 3 + k + 7 + 2000; }

// ---------------------------------------------------------------------------------------------
// LCD console (ST7365P on SPI2, from PicoMultiLoader's lcd.c): 6x10 font, 53 x 32 characters,
// scrolls up when full, flushed after every line so the last line survives a crash.

#define LCD_W 320
#define LCD_H 320
#define CON_COLS (LCD_W / 6)
#define CON_ROWS (LCD_H / 10)

static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_fb;   // byte-swapped RGB565
static SemaphoreHandle_t s_lcdDone;
static int s_conRow;

static const uint8_t kLcdInit[] = {
  0xE0, 15, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,
  0xE1, 15, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,
  0xC0, 2, 0x17, 0x15,
  0xC1, 1, 0x41,
  0xC5, 3, 0x00, 0x12, 0x80,
  0x36, 1, 0x48,   // MADCTL: MX | BGR
  0x3A, 1, 0x55,   // RGB565
  0xB0, 1, 0x00,
  0xB1, 1, 0xA0,
  0x21, 0,         // INVON
  0xB4, 1, 0x02,
  0xB6, 3, 0x02, 0x02, 0x3B,
  0xB7, 1, 0xC6,
  0xE9, 1, 0x00,
  0xF7, 4, 0xA9, 0x51, 0x2C, 0x82,
};

static bool lcdColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_lcdDone, &woken);
  return woken == pdTRUE;
}

static bool lcdInit() {
  // GPIO24/25 (DC/RST) come out of reset owned by the USB-Serial-JTAG PHY.
  USB_SERIAL_JTAG.conf0.pad_pull_override = 1;
  USB_SERIAL_JTAG.conf0.dp_pullup = 0;
  USB_SERIAL_JTAG.conf0.dm_pullup = 0;
  usb_serial_jtag_ll_phy_enable_pad(false);

  gpio_config_t out = {};
  out.pin_bit_mask = (1ULL << 7) | (1ULL << 25);
  out.mode = GPIO_MODE_OUTPUT;
  gpio_config(&out);
  gpio_set_level((gpio_num_t)7, 0);   // CS held low for good

  spi_bus_config_t bus = {};
  bus.sclk_io_num = 3;
  bus.mosi_io_num = 2;
  bus.miso_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = LCD_W * 32 * 2;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  esp_lcd_panel_io_spi_config_t io = {};
  io.dc_gpio_num = 24;
  io.cs_gpio_num = -1;
  io.pclk_hz = 40 * 1000 * 1000;
  io.spi_mode = 0;
  io.trans_queue_depth = 4;
  io.lcd_cmd_bits = 8;
  io.lcd_param_bits = 8;
  io.on_color_trans_done = lcdColorDone;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &s_io) != ESP_OK) return false;

  s_lcdDone = xSemaphoreCreateBinary();
  s_fb = (uint16_t *)heap_caps_aligned_calloc(128, LCD_W * LCD_H, sizeof(uint16_t), MALLOC_CAP_DMA);
  if (!s_fb) return false;

  gpio_set_level((gpio_num_t)25, 1); delay(10);
  gpio_set_level((gpio_num_t)25, 0); delay(10);
  gpio_set_level((gpio_num_t)25, 1); delay(200);
  for (size_t i = 0; i < sizeof kLcdInit;) {
    uint8_t cmd = kLcdInit[i++], n = kLcdInit[i++];
    esp_lcd_panel_io_tx_param(s_io, cmd, n ? &kLcdInit[i] : NULL, n);
    i += n;
  }
  esp_lcd_panel_io_tx_param(s_io, 0x11, NULL, 0);   // sleep out
  delay(120);
  esp_lcd_panel_io_tx_param(s_io, 0x29, NULL, 0);   // display on
  delay(20);
  return true;
}

static void lcdFlush() {
  if (!s_fb) return;
  static const uint8_t ca[4] = {0, 0, (LCD_W - 1) >> 8, (LCD_W - 1) & 0xFF};
  static const uint8_t ra[4] = {0, 0, (LCD_H - 1) >> 8, (LCD_H - 1) & 0xFF};
  esp_lcd_panel_io_tx_param(s_io, 0x2A, ca, 4);
  esp_lcd_panel_io_tx_param(s_io, 0x2B, ra, 4);
  esp_lcd_panel_io_tx_color(s_io, 0x2C, s_fb, LCD_W * LCD_H * 2);
  xSemaphoreTake(s_lcdDone, portMAX_DELAY);
}

static void conPutLine(const char *s, uint16_t color) {
  if (!s_fb) return;
  uint16_t c = (uint16_t)((color << 8) | (color >> 8));
  do {
    if (s_conRow == CON_ROWS) {   // scroll up one text row
      memmove(s_fb, s_fb + LCD_W * 10, LCD_W * (LCD_H - 10) * 2);
      memset(s_fb + LCD_W * (LCD_H - 10), 0, LCD_W * 10 * 2);
      s_conRow--;
    }
    int y = s_conRow++ * 10;
    for (int col = 0; col < CON_COLS && *s; col++, s++) {
      uint8_t ch = (uint8_t)*s;
      if (ch < 0x20 || ch > 0x7E) ch = '?';
      const uint8_t *g = &UIFONT_6X10[(ch - 0x20) * 10];
      for (int r = 0; r < 10; r++)
        for (int b = 0; b < 6; b++)
          if (g[r] & (0x80 >> b)) s_fb[(y + r) * LCD_W + col * 6 + b] = c;
    }
  } while (*s);   // wrap long lines
  lcdFlush();
}

// ---------------------------------------------------------------------------------------------
// Logging to serial, LCD and SD. Each line is appended to the SD file at once, so a crash
// still leaves everything up to the failing step.

static SPIClass s_sdSpi(HSPI);
static bool s_sdOk;
static int s_fail, s_pass;

// Survives a panic reset (not a power cycle): the step that was running when it crashed.
#define STEP_MAGIC 0x50535450u
static __NOINIT_ATTR uint32_t s_stepMagic;
static __NOINIT_ATTR char s_step[96];

static void step(const char *what) {
  s_stepMagic = STEP_MAGIC;
  snprintf(s_step, sizeof s_step, "%s", what);
}

static void logf(const char *fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Serial.println(buf);
  uint16_t color = 0xFFFF;
  if (!strncmp(buf, "PASS", 4)) color = 0x07E0;
  else if (!strncmp(buf, "FAIL", 4) || !strncmp(buf, "CRASH", 5)) color = 0xF800;
  else if (!strncmp(buf, "INFO", 4) || !strncmp(buf, "---", 3) || !strncmp(buf, "===", 3)) color = 0xFFE0;
  conPutLine(buf, color);
  if (s_sdOk) {
    File f = SD.open("/picocalc_p0.txt", FILE_APPEND);
    if (f) { f.println(buf); f.close(); }
  }
}

static void check(bool ok, const char *what) {
  logf("%s  %s", ok ? "PASS" : "FAIL", what);
  if (ok) s_pass++; else s_fail++;
}

static void sdInit() {
  esp_ldo_channel_handle_t ldo = nullptr;
  esp_ldo_channel_config_t c = {};
  c.chan_id = 4;            // SD IO domain, off at boot
  c.voltage_mv = 3300;
  esp_ldo_acquire_channel(&c, &ldo);
  s_sdSpi.begin(46, 48, 33, 47);
  for (int i = 0; i < 10 && !s_sdOk; i++) {
    s_sdOk = SD.begin(47, s_sdSpi, 20000000, "/sd");
    if (!s_sdOk) { SD.end(); delay(120); }
  }
}

// ---------------------------------------------------------------------------------------------
// Flash write guard: allow only the free tail of the running partition.

static const esp_flash_os_functions_t *s_origOs;
static esp_flash_os_functions_t s_os;
static uint32_t s_allowLo, s_allowHi;

static esp_err_t IRAM_ATTR tailRegionProtected(void *arg, size_t addr, size_t size) {
  if (addr >= s_allowLo && addr + size <= s_allowHi) return ESP_OK;
  return s_origOs->region_protected ? s_origOs->region_protected(arg, addr, size) : ESP_OK;
}

static void allowTailWrites(uint32_t lo, uint32_t hi) {
  s_allowLo = lo;
  s_allowHi = hi;
  if (!s_origOs) {
    s_origOs = esp_flash_default_chip->os_func;
    s_os = *s_origOs;
    s_os.region_protected = tailRegionProtected;
    esp_flash_default_chip->os_func = &s_os;
  }
}

// ---------------------------------------------------------------------------------------------

extern "C" int _instruction_reserved_end, _rodata_reserved_end;

static bool writeProbe(const esp_partition_t *p, uint32_t off, const uint32_t *code, size_t bytes, uint32_t *us) {
  uint32_t t0 = micros();
  if (esp_partition_erase_range(p, off, 0x10000) != ESP_OK) return false;
  if (esp_partition_write(p, off, code, bytes) != ESP_OK) return false;
  *us = micros() - t0;
  return true;
}

static void spikeA() {
  logf("--- (a) execute from ota_0 tail ---");
  const esp_partition_t *p = esp_ota_get_running_partition();
  logf("running partition '%s' at 0x%06lx size 0x%06lx", p->label, (unsigned long)p->address, (unsigned long)p->size);

  esp_partition_pos_t pos = { p->address, p->size };
  esp_image_metadata_t meta;
  if (esp_image_get_metadata(&pos, &meta) != ESP_OK) { check(false, "esp_image_get_metadata"); return; }
  uint32_t tail = (meta.image_len + 0xFFFF) & ~0xFFFFu;
  logf("image_len %lu, code tail at partition offset 0x%06lx (%lu KB free)", (unsigned long)meta.image_len,
       (unsigned long)tail, (unsigned long)((p->size - tail) >> 10));
  if (tail + 0x10000 > p->size) { check(false, "room for a 64 KB tail"); return; }

  // Unwrapped, IDF would abort() here. Prove the wrapper is narrow: image start stays protected.
  allowTailWrites(p->address + tail, p->address + p->size);
  check(tailRegionProtected(esp_flash_default_chip->os_func_data, p->address, 4096) != ESP_OK,
        "write guard still protects the image itself");
  check(tailRegionProtected(esp_flash_default_chip->os_func_data, p->address + tail, 4096) == ESP_OK,
        "write guard allows the tail");

  uint32_t us;
  step("(a) erase + write v1 into ota_0 tail");
  check(writeProbe(p, tail, PROBE_V1, sizeof PROBE_V1, &us), "erase + write v1 into tail");
  logf("  64 KB erase + write took %lu us", (unsigned long)us);

  const void *ptr;
  esp_partition_mmap_handle_t h;
  step("(a) esp_partition_mmap(INST)");
  esp_err_t e = esp_partition_mmap(p, tail, 0x10000, ESP_PARTITION_MMAP_INST, &ptr, &h);
  check(e == ESP_OK, "esp_partition_mmap(INST)");
  if (e != ESP_OK) { logf("  err %s", esp_err_to_name(e)); return; }
  uint32_t irom_end = ((uint32_t)&_instruction_reserved_end + 0xFFFF) & ~0xFFFFu;
  uint32_t drom_end = ((uint32_t)&_rodata_reserved_end + 0xFFFF) & ~0xFFFFu;
  logf("  mapped at %p (firmware irom end 0x%08lx, drom end 0x%08lx)", ptr, (unsigned long)irom_end,
       (unsigned long)drom_end);
  check((uint32_t)ptr >= drom_end, "mapping lies above the PMP-fenced firmware ranges");
  step("(a) data read through the mapping");
  check(((const uint32_t *)ptr)[14] == 1000, "data read through the mapping");

  step("(a) call into the mapped code (v1)");
  __asm__ volatile("fence.i" ::: "memory");
  s_cbCalls = 0;
  int r = ((probe_fn)ptr)(5, cbTriple, 10);
  logf("  v1 probe(5, cb, 10) = %d (expect %d), callbacks %d", r, expectV1(5, 10), s_cbCalls);
  check(r == expectV1(5, 10) && s_cbCalls == 1, "call into flash code + callback into firmware");

  // Rewrite after unmapping: the runtime's normal "build, run again" path.
  step("(a) rewrite with v2 + remap + call");
  esp_partition_munmap(h);
  check(writeProbe(p, tail, PROBE_V2, sizeof PROBE_V2, &us), "rewrite tail with v2");
  e = esp_partition_mmap(p, tail, 0x10000, ESP_PARTITION_MMAP_INST, &ptr, &h);
  __asm__ volatile("fence.i" ::: "memory");
  r = e == ESP_OK ? ((probe_fn)ptr)(5, cbTriple, 10) : -1;
  logf("  v2 probe(5, cb, 10) = %d (expect %d)", r, expectV2(5, 10));
  check(r == expectV2(5, 10), "remap after rewrite runs the new code (no stale cache)");

  // Informational: rewrite while still mapped. The runtime avoids this, but it is good to know.
  step("(a) rewrite while mapped (informational)");
  uint32_t us2;
  bool w = writeProbe(p, tail, PROBE_V1, sizeof PROBE_V1, &us2);
  __asm__ volatile("fence.i" ::: "memory");
  r = w ? ((probe_fn)ptr)(5, cbTriple, 10) : -1;
  logf("INFO  rewrite while mapped: got %d (v1 %d, stale v2 %d)", r, expectV1(5, 10), expectV2(5, 10));
  esp_partition_munmap(h);

  // Timing for a run of 10 back-to-back calls (cache warm).
  step("(a) timing loop");
  uint32_t t0 = micros();
  e = esp_partition_mmap(p, tail, 0x10000, ESP_PARTITION_MMAP_INST, &ptr, &h);
  for (int i = 0; i < 10 && e == ESP_OK; i++) r = ((probe_fn)ptr)(i, cbTriple, 0);
  logf("  mmap + 10 calls: %lu us", (unsigned long)(micros() - t0));
  if (e == ESP_OK) esp_partition_munmap(h);
}

static void spikeB() {
  logf("--- (b) export slot ---");
  const volatile uint8_t *s = pc_slot;
  logf("pc_slot at %p, size 1 MB", (const void *)pc_slot);
  check((uint32_t)pc_slot < (uint32_t)&_instruction_reserved_end, "slot linked inside .flash.text");
  const SlotHeader *hdr = (const SlotHeader *)pc_slot;
  if (hdr->magic == SLOT_MAGIC) {
    logf("slot: PATCHED, entry +0x%lx, flags 0x%lx", (unsigned long)hdr->entry, (unsigned long)hdr->flags);
    step("(b) call into the patched slot");
    __asm__ volatile("fence.i" ::: "memory");
    s_cbCalls = 0;
    int r = ((probe_fn)(pc_slot + hdr->entry))(5, cbTriple, 10);
    logf("  slot probe(5, cb, 10) = %d (expect %d)", r, expectV1(5, 10));
    check(r == expectV1(5, 10) && s_cbCalls == 1, "patched image boots and runs the slot code");
  } else {
    uint32_t ff = 0;
    step("(b) sample the empty slot");
    for (int i = 0; i < 0x100000; i += 4096) ff += s[i] == 0xFF;
    logf("slot: EMPTY (sampled 256 pages, %lu are 0xFF)", (unsigned long)ff);
    check(ff == 256, "empty slot is all 0xFF");
  }
}

void setup() {
  Serial.begin(115200);
  bool lcdOk = lcdInit();
  sdInit();
  const esp_app_desc_t *d = esp_app_get_description();
  logf("=== spike_p0 %s %s, lcd %s, sd %s ===", d->date, d->time, lcdOk ? "ok" : "FAIL",
       s_sdOk ? "ok" : "MISSING");
  logf("heap internal %u KB, psram %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));

  // A panic resets straight back into this app. Report the step that crashed instead of
  // crashing again in a loop.
  esp_reset_reason_t why = esp_reset_reason();
  bool crashed = s_stepMagic == STEP_MAGIC && why != ESP_RST_POWERON && why != ESP_RST_SW;
  if (crashed) {
    s_step[sizeof s_step - 1] = 0;
    logf("CRASH  previous run died in: %s (reset reason %d)", s_step, (int)why);
    s_fail++;
  }
  s_stepMagic = 0;

  bool skipA = crashed && s_step[1] == 'a', skipB = crashed && s_step[1] == 'b';
  if (!skipB) spikeB();   // first: it doesn't touch flash
  else logf("INFO  skipping (b) after the crash; power-cycle to retry it");
  if (!skipA) spikeA();
  else logf("INFO  skipping (a) after the crash; power-cycle to retry it");
  s_stepMagic = 0;
  logf("=== %d passed, %d failed ===", s_pass, s_fail);
  logf("Log: /sd/picocalc_p0.txt. Back to the loader: hold");
  logf("Up/F1/F5 and reset.");
}

void loop() {
  static uint32_t last;
  if (millis() - last > 10000) {
    last = millis();
    Serial.printf("spike_p0 done: %d passed, %d failed (log in /sd/picocalc_p0.txt)\n", s_pass, s_fail);
  }
  delay(50);
}
