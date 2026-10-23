// sys.cpp - picocalc.h sys_*, SDK start-up / reset and the program exit path.
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include "picocalc.h"
#include "sdk_internal.h"
#include "../hal/hal.h"

static uint32_t s_rng = 1;
static uint32_t s_lastSleep;

uint32_t sys_millis(void) { return hal_millis(); }
uint64_t sys_micros(void) { return hal_micros(); }

// Sleep in short slices so Break still ends the program during a long delay.
void sys_delay(uint32_t ms) {
  const uint32_t end = hal_millis() + ms;
  sdk_kbd_pump();
  for (;;) {
    const int32_t left = (int32_t)(end - hal_millis());
    if (left <= 0) break;
    hal_delay(left < 10 ? (uint32_t)left : 10);
    sdk_kbd_pump();
  }
  s_lastSleep = hal_millis();
}

// A busy program that only calls sys_yield() still gives the idle task a tick now and then, or
// the task watchdog fires.
void sys_yield(void) {
  sdk_kbd_pump();
  if (hal_millis() - s_lastSleep >= 100) {
    vTaskDelay(1);
    s_lastSleep = hal_millis();
  } else {
    taskYIELD();
  }
}

int sys_battery(bool *charging) {
  if (charging) *charging = hal_battery_charging();
  return hal_battery_percent();
}

void sys_backlight(uint8_t level) { hal_lcd_backlight(level); }

uint32_t sys_random(void) {       // xorshift32
  uint32_t x = s_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return s_rng = x;
}

void sys_seed(uint32_t seed) { s_rng = seed ? seed : 1; }

size_t sys_free_heap(void) { return hal_free_heap(); }

void sys_log(const char *fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  hal_log("%s", b);
}

void sys_exit(void) { sdk_exit(); }
void sys_exit_to_loader(void) {
  sdk_reset();
  hal_reboot_to_loader();
  for (;;) {}
}

// ---- start-up, reset, exit ----

// Without a runtime there is nothing to return to: say so, then restart the firmware (which runs
// the program again) or go back to the loader.
static void defaultExit() {
  snd_stop();
  gfx_clip(0, 0, SCREEN_W, SCREEN_H);
  gfx_font(FONT_6X10);
  gfx_fill_rect(0, SCREEN_H - 24, SCREEN_W, 24, DARKGRAY);
  gfx_text(4, SCREEN_H - 20, "Program ended.", WHITE);
  gfx_text(4, SCREEN_H - 10, "Any key: restart   Esc: loader", LIGHTGRAY);
  gfx_flush();
  kbd_flush();
  HalKey k;
  for (;;) {
    while (hal_key_poll(&k)) {
      if (k.state != HAL_KEY_PRESSED) continue;
      if (k.code == KEY_ESC) hal_reboot_to_loader();
      if (k.code != KEY_BREAK) hal_reboot();
    }
    hal_delay(10);
  }
}

static sdk_exit_fn s_exitFn = defaultExit;

sdk_exit_fn sdk_set_exit_handler(sdk_exit_fn fn) {
  sdk_exit_fn old = s_exitFn;
  s_exitFn = fn ? fn : defaultExit;
  return old;
}

void sdk_exit() {
  s_exitFn();
  for (;;) hal_delay(1000);          // a handler must not return; if it does, park here
}

void sdk_init() {
  sdk_gfx_init();
  sdk_snd_init();
  sys_seed(hal_random());
  sdk_reset();
}

void sdk_reset() {
  sdk_gfx_reset();
  sdk_kbd_reset();
  sdk_snd_reset();
  s_lastSleep = hal_millis();
}
