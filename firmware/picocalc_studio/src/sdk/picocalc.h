/*
 * picocalc.h - PicoCalc Studio SDK (ESP32-P4 PicoCalc)
 *
 * Programs are written Arduino-style:
 *
 *     #include <picocalc.h>
 *     void setup(void) { ... }   // called once
 *     void loop(void)  { ... }   // called repeatedly until sys_exit()
 *
 * A plain `int main(void)` is also accepted; the program ends when it returns.
 * printf() output goes to the IDE console and the serial port.
 *
 * Every function declared here is exported by the firmware; the on-device
 * linker resolves calls against that export table.
 */
#ifndef PICOCALC_H
#define PICOCALC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ screen */

#define SCREEN_W 320
#define SCREEN_H 320

typedef uint16_t color_t;                       /* RGB565 */

#define RGB(r, g, b) ((color_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xFF) >> 3)))

#define BLACK     0x0000
#define WHITE     0xFFFF
#define RED       0xF800
#define GREEN     0x07E0
#define BLUE      0x001F
#define YELLOW    0xFFE0
#define CYAN      0x07FF
#define MAGENTA   0xF81F
#define ORANGE    0xFD20
#define GRAY      0x8410
#define DARKGRAY  0x4208
#define LIGHTGRAY 0xC618
#define NAVY      0x000F
#define DARKGREEN 0x03E0
#define MAROON    0x7800
#define PURPLE    0x780F

enum {
    FONT_6X10 = 0,   /* default: 53 x 32 characters */
    FONT_7X13 = 1,   /* 45 x 24 characters */
    FONT_8X8  = 2,   /* 40 x 40 characters */
};

/*
 * All drawing goes to an off-screen framebuffer (320x320 RGB565 in PSRAM).
 * Nothing appears until gfx_flush() / gfx_flush_rect() is called.
 * Coordinates outside the screen (or the clip rectangle) are clipped.
 */
void     gfx_clear(color_t c);
void     gfx_pixel(int x, int y, color_t c);
color_t  gfx_get_pixel(int x, int y);
void     gfx_hline(int x, int y, int w, color_t c);
void     gfx_vline(int x, int y, int h, color_t c);
void     gfx_line(int x0, int y0, int x1, int y1, color_t c);
void     gfx_rect(int x, int y, int w, int h, color_t c);
void     gfx_fill_rect(int x, int y, int w, int h, color_t c);
void     gfx_round_rect(int x, int y, int w, int h, int r, color_t c);
void     gfx_fill_round_rect(int x, int y, int w, int h, int r, color_t c);
void     gfx_circle(int cx, int cy, int r, color_t c);
void     gfx_fill_circle(int cx, int cy, int r, color_t c);
void     gfx_triangle(int x0, int y0, int x1, int y1, int x2, int y2, color_t c);
void     gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, color_t c);
void     gfx_blit(int x, int y, int w, int h, const color_t *pixels);
void     gfx_scroll(int dy, color_t fill);            /* scroll whole screen up (dy>0) */

void     gfx_clip(int x, int y, int w, int h);
void     gfx_noclip(void);

void     gfx_font(int font);
int      gfx_font_w(void);
int      gfx_font_h(void);
/* Text: returns the x position after the last character. bg = -1 means transparent. */
int      gfx_text(int x, int y, const char *s, color_t fg);
int      gfx_text_bg(int x, int y, const char *s, color_t fg, int bg);
int      gfx_printf(int x, int y, color_t fg, const char *fmt, ...);
int      gfx_text_width(const char *s);

void     gfx_flush(void);                             /* push whole framebuffer to LCD */
void     gfx_flush_rect(int x, int y, int w, int h);
color_t *gfx_framebuffer(void);                       /* direct access, row-major, 320 px stride */

/* ---------------------------------------------------------------- keyboard */

/* Printable keys arrive as ASCII with Shift/Caps already applied. */
#define KEY_NONE      0x00
#define KEY_BACKSPACE 0x08
#define KEY_TAB       0x09
#define KEY_ENTER     0x0A
#define KEY_SPACE     0x20
#define KEY_F1        0x81
#define KEY_F2        0x82
#define KEY_F3        0x83
#define KEY_F4        0x84
#define KEY_F5        0x85
#define KEY_F6        0x86
#define KEY_F7        0x87
#define KEY_F8        0x88
#define KEY_F9        0x89
#define KEY_F10       0x90
#define KEY_ALT       0xA1
#define KEY_SHIFT_L   0xA2
#define KEY_SHIFT_R   0xA3
#define KEY_SYM       0xA4
#define KEY_CTRL      0xA5
#define KEY_ESC       0xB1
#define KEY_LEFT      0xB4
#define KEY_UP        0xB5
#define KEY_DOWN      0xB6
#define KEY_RIGHT     0xB7
#define KEY_CAPS      0xC1
#define KEY_BREAK     0xD0
#define KEY_INSERT    0xD1
#define KEY_HOME      0xD2
#define KEY_DEL       0xD4
#define KEY_END       0xD5
#define KEY_PGUP      0xD6
#define KEY_PGDN      0xD7

#define KEY_PRESSED   1
#define KEY_HELD      2
#define KEY_RELEASED  3

#define MOD_SHIFT 0x01
#define MOD_CTRL  0x02
#define MOD_ALT   0x04
#define MOD_SYM   0x08

typedef struct {
    uint8_t code;     /* KEY_* or ASCII */
    uint8_t state;    /* KEY_PRESSED / KEY_HELD / KEY_RELEASED */
    uint8_t mods;     /* MOD_* held when the event happened */
    uint8_t _pad;
} key_event_t;

int  kbd_get(void);                 /* next key press (with auto-repeat), KEY_NONE if none */
int  kbd_wait(void);                /* blocks until a key is pressed */
bool kbd_event(key_event_t *ev);    /* raw events incl. releases; false if queue empty */
bool kbd_down(int code);            /* is the key currently held */
int  kbd_mods(void);                /* MOD_* bitmask of currently held modifiers */
void kbd_flush(void);               /* discard pending events */
void kbd_backlight(uint8_t level);  /* keyboard backlight 0..255 */

/* ------------------------------------------------------------------- sound */

void snd_volume(int level);                         /* 0..10 */
void snd_tone(int freq_hz, int ms);                 /* square wave, non-blocking */
void snd_stop(void);
bool snd_busy(void);
/* Queue mono signed 16-bit samples at any rate; blocks while the queue is full. */
int  snd_play(const int16_t *samples, int count, int rate_hz);

/* ------------------------------------------------------------------ system */

uint32_t sys_millis(void);
uint64_t sys_micros(void);
void     sys_delay(uint32_t ms);
void     sys_yield(void);                 /* let the system run (keyboard, Break check) */
int      sys_battery(bool *charging);     /* percent 0..100, charging may be NULL */
void     sys_backlight(uint8_t level);    /* LCD backlight 0..255 */
uint32_t sys_random(void);
void     sys_seed(uint32_t seed);
size_t   sys_free_heap(void);
void     sys_log(const char *fmt, ...);   /* serial port only */
void     sys_exit(void) __attribute__((noreturn));          /* end program, back to IDE */
void     sys_exit_to_loader(void) __attribute__((noreturn)); /* reboot into PicoMultiLoader */

/* Paths: the SD card is mounted at "/sd". Standard <stdio.h> and <dirent.h> work on it. */
#define SD_ROOT "/sd"

#ifdef __cplusplus
}
#endif

#endif /* PICOCALC_H */
