// input.cpp - the PicoCalc keyboard: an STM32 at I2C 0x1F that also owns both backlights and
// reads the battery gauge. Protocol and timings from pico-386 / emu16 (input_p4.cpp, from emu8):
//
//  * FIFO poll is split-phase: writing register 0x09 selects (and dequeues) the next event; a
//    2-byte read some time later returns {state, code}, {0,0} when empty. At the 10kHz the STM32
//    needs, one read costs ~3ms, so a task on core 0 polls every 10ms and hands events over
//    through a queue. Nothing else touches the bus: backlight changes are queued for that task too.
//  * The battery register 0x0B rides the same poll every 10s, only in place of a FIFO select that
//    came back empty: {0x0B, v}, percent = v & 0x7F, bit 7 = charging.
//  * Backlights: write (reg | 0x80, level); 0x0A = LCD, 0x05 = keyboard.
#include "../../../board.h"
#include <Arduino.h>
#include <Wire.h>
#include "../hal.h"

#define KBD_REG_FIF   0x09
#define KBD_REG_BAT   0x0B
#define KBD_REG_BKL   0x05        // keyboard backlight
#define KBD_REG_BK2   0x0A        // LCD backlight
#define KBD_REG_WRITE 0x80
#define BAT_POLL_MS   10000

void p4CheckSerialCmd();          // system.cpp

static QueueHandle_t s_keys = nullptr;
static bool s_wireUp = false;
static bool s_selPending = false, s_batPending = false;
static volatile int  s_batPercent = -1;
static volatile bool s_batCharging = false;
static volatile int  s_lcdBkl = -1, s_kbdBkl = -1;   // pending backlight writes, -1 = none
static bool s_headless = false;

static void wireBegin() {
  if (s_wireUp) return;
  Wire.begin(KBD_I2C_SDA_PIN, KBD_I2C_SCL_PIN, KBD_I2C_HZ);
  s_wireUp = true;
}

static void selectReg(uint8_t reg) {
  Wire.beginTransmission(KBD_I2C_ADDR);
  Wire.write(reg);
  const bool ok = Wire.endTransmission() == 0;
  s_selPending = ok && reg == KBD_REG_FIF;
  s_batPending = ok && reg == KBD_REG_BAT;
}

static void readBat() {
  s_batPending = false;
  if (Wire.requestFrom((uint8_t)KBD_I2C_ADDR, (size_t)2) != 2) return;
  const uint8_t r = (uint8_t)Wire.read(), v = (uint8_t)Wire.read();
  if (r != KBD_REG_BAT || (v & 0x7F) > 100) return;
  s_batPercent = v & 0x7F;
  s_batCharging = (v & 0x80) != 0;
}

// Selecting the FIFO already dequeued an event, so a failed read loses it -- possibly a release,
// which would leave a key held for good. Then (and when our own queue overflows) a
// HAL_KEY_RELEASE_ALL goes out instead; a key still down is picked up again on its next press.
static bool s_lost = false;

static void flushLost() {
  if (!s_lost) return;
  const HalKey all = { HAL_KEY_RELEASE_ALL, HAL_KEY_RELEASED };
  if (xQueueSend(s_keys, &all, 0) == pdTRUE) s_lost = false;
}

static void (*s_breakHook)(void);
void hal_set_break_hook(void (*hook)(void)) { s_breakHook = hook; }

static void sendKey(uint8_t code, uint8_t state) {
  if (code == HAL_KEY_CODE_BREAK && state == HAL_KEY_PRESSED && s_breakHook) s_breakHook();
  flushLost();
  if (s_lost) return;                    // still full: this one is covered by the release-all
  const HalKey ev = { code, state };
  if (xQueueSend(s_keys, &ev, 0) != pdTRUE) s_lost = true;
}

static bool readFifo(uint8_t *state, uint8_t *code) {
  if (!s_selPending) return false;
  s_selPending = false;
  if (Wire.requestFrom((uint8_t)KBD_I2C_ADDR, (size_t)2) != 2) { s_lost = true; return false; }
  const uint8_t s = (uint8_t)Wire.read(), c = (uint8_t)Wire.read();
  if (s == 0 && c == 0) return false;
  *state = s; *code = c;
  return true;
}

static bool mainboardPresent() {
  wireBegin();
  Wire.beginTransmission(KBD_I2C_ADDR);
  return Wire.endTransmission() == 0;
}

// The module has its own USB power, so the PicoCalc's switch does not reset it. If the mainboard
// is off at boot, nothing on it (panel, SD, keyboard) can be set up: wait for it a while.
bool p4WaitForMainboard() {
  if (mainboardPresent()) return true;
  hal_log("Waiting for the PicoCalc mainboard - turn the unit on");
  const uint32_t t0 = millis();
  while (millis() - t0 < PICOCALC_MAINBOARD_WAIT_MS) {
    if (mainboardPresent()) { delay(300); return true; }
    delay(100);
  }
  hal_log("mainboard never came up; will reboot when it does");
  s_headless = true;
  return false;
}

static bool bklWrite(uint8_t reg, uint8_t level) {
  Wire.beginTransmission(KBD_I2C_ADDR);
  Wire.write((uint8_t)(reg | KBD_REG_WRITE));
  Wire.write(level);
  return Wire.endTransmission() == 0;
}

// Every poll ends with a register select whose answer is read on the next tick. Backlight writes
// go just before that select, when no answer is outstanding: a write in between would make the
// STM32 answer for the backlight register and the dequeued key event would be lost.
static void selectNext(uint8_t reg) {
  int v = s_lcdBkl;
  if (v >= 0) { s_lcdBkl = -1; bklWrite(KBD_REG_BK2, (uint8_t)v); }
  v = s_kbdBkl;
  if (v >= 0) { s_kbdBkl = -1; bklWrite(KBD_REG_BKL, (uint8_t)v); }
  selectReg(reg);
}

static void kbdTask(void *) {
  uint32_t lastBat = 0;
  bool batOnce = false;
  selectReg(KBD_REG_FIF);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10));
    p4CheckSerialCmd();
    if (s_headless) {
      if (mainboardPresent()) { hal_log("mainboard is up - rebooting"); hal_reboot(); }
      continue;
    }
    if (s_batPending) { readBat(); selectNext(KBD_REG_FIF); continue; }
    uint8_t state, code;
    // Drain a few events per tick so a burst (several keys at once) is not spread over frames.
    int got = 0;
    while (got < 4 && readFifo(&state, &code)) {
      sendKey(code, state);
      got++;
      selectReg(KBD_REG_FIF);
      delayMicroseconds(2000);           // STM32 turnaround after a select
    }
    flushLost();
    if (got) {                           // after 4 events a select is still outstanding: keep it
      if (!s_selPending) selectNext(KBD_REG_FIF);
      continue;
    }
    const uint32_t now = millis();
    if (!batOnce || now - lastBat >= BAT_POLL_MS) {   // only in place of an empty FIFO select
      batOnce = true; lastBat = now;
      selectNext(KBD_REG_BAT);
    } else {
      selectNext(KBD_REG_FIF);
    }
  }
}

void p4InputInit() {
  wireBegin();
  s_keys = xQueueCreate(64, sizeof(HalKey));
  if (!s_headless) {
    bklWrite(KBD_REG_BK2, 0xFF);
    bklWrite(KBD_REG_BKL, 0x80);
  }
  xTaskCreatePinnedToCore(kbdTask, "kbd", 4096, nullptr, 4, nullptr, 0);
}

bool hal_key_poll(HalKey *ev) { return s_keys && xQueueReceive(s_keys, ev, 0) == pdTRUE; }
void hal_key_inject(uint8_t code, uint8_t state) {
  if (code == HAL_KEY_CODE_BREAK && state == HAL_KEY_PRESSED && s_breakHook) s_breakHook();
  const HalKey ev = { code, state };
  if (s_keys) xQueueSend(s_keys, &ev, 0);
}
int  hal_battery_percent()           { return s_batPercent; }
bool hal_battery_charging()          { return s_batCharging; }
void hal_lcd_backlight(uint8_t level) { s_lcdBkl = level; }
void hal_kbd_backlight(uint8_t level) { s_kbdBkl = level; }
