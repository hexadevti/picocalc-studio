// hal.h - what the SDK needs from the machine. One implementation per board: src/hal/picocalc_p4
// today, an RP2040/RP2350 one later. The SDK (src/sdk) only talks to the hardware through this.
//
// Pixels are native-endian RGB565. Key codes are the PicoCalc keyboard's own (picocalc.h KEY_*).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define HAL_KEY_PRESSED   1
#define HAL_KEY_HELD      2
#define HAL_KEY_RELEASED  3
#define HAL_KEY_RELEASE_ALL 0x00   // code with HAL_KEY_RELEASED: events were lost, treat every key as up
#define HAL_KEY_CODE_BREAK  0xD0   // picocalc.h KEY_BREAK

struct HalKey {
  uint8_t code;
  uint8_t state;   // HAL_KEY_*
};

// lifecycle / system
void     hal_init();                       // everything below is usable afterwards
uint32_t hal_millis();
uint64_t hal_micros();
void     hal_delay(uint32_t ms);
void     hal_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void     hal_reboot();
void     hal_reboot_to_loader();           // PicoMultiLoader's menu
void    *hal_alloc_big(size_t n);          // PSRAM (zeroed)
void    *hal_alloc_fast(size_t n);         // internal RAM if it fits, else PSRAM (zeroed)
void     hal_free(void *p);
size_t   hal_free_heap();
uint32_t hal_random();                     // hardware RNG
// Crash breadcrumbs: survive a panic reset. hal_crash_report() describes the last crash once.
void     hal_breadcrumb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool     hal_crash_report(char *buf, size_t n);
// Code addresses of the last panic (pc, then ra when known, then return addresses found on the stack), without
// consuming the report. Returns how many; 0 if the last reset wasn't a recorded panic.
int      hal_crash_pcs(uint32_t *pcs, int max);
// Serial "@..." lines the HAL doesn't know go here (from the keyboard task: don't block in it).
void     hal_set_serial_cmd_hook(bool (*hook)(const char *line));

// display: synchronous rectangle push / fill
void hal_lcd_push(int x, int y, int w, int h, const uint16_t *px, int stride);
void hal_lcd_fill(int x, int y, int w, int h, uint16_t color);

// keyboard: events from the keyboard task, oldest first
bool hal_key_poll(HalKey *ev);
void hal_key_inject(uint8_t code, uint8_t state);   // serial @key, tests
// Called from the keyboard task on every Break press, besides queueing it: lets the runtime stop
// a program that never reads the keyboard. Must not block.
void hal_set_break_hook(void (*hook)(void));

// power: battery from the keyboard MCU (-1 until the first reading), backlights 0..255
int  hal_battery_percent();
bool hal_battery_charging();
void hal_lcd_backlight(uint8_t level);
void hal_kbd_backlight(uint8_t level);

// audio: mono signed 16-bit at any rate, resampled to the output. Writes block while the output
// queue is full.
void     hal_audio_start(uint32_t rate);
void     hal_audio_write(const int16_t *mono, int n);
void     hal_audio_volume(int vol);        // 0..10
uint32_t hal_audio_queued_us();            // written but not played yet

// storage: plain stdio/dirent below this root
bool        hal_sd_mounted();
const char *hal_sd_root();
