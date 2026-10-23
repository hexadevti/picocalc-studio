// kbd.cpp - picocalc.h kbd_*: raw events, held-key state, modifiers and auto-repeating presses on
// top of the HAL's key event queue. The keyboard's own firmware already applies Shift/Caps to
// printable keys, so codes arrive as the ASCII the user sees.
//
// Every kbd_* call (and sys_yield / sys_delay) pumps the HAL queue into two rings: all events
// for kbd_event(), presses only for kbd_get(). A ring that the program doesn't read drops its
// oldest entries. Break is not delivered: it ends the program.
#include <string.h>
#include "picocalc.h"
#include "sdk_internal.h"
#include "../hal/hal.h"

#define REPEAT_DELAY_MS  400
#define REPEAT_PERIOD_MS 60
#define RING             32

static key_event_t s_ev[RING];
static uint8_t s_evHead, s_evLen;
static uint8_t s_press[RING];
static uint8_t s_prHead, s_prLen;
static uint32_t s_down[8];          // bitmap of held codes
static int s_repeatKey = KEY_NONE;
static uint32_t s_repeatAt;

static inline bool isDown(int c) { return (s_down[c >> 5] >> (c & 31)) & 1; }
static inline void setDown(int c, bool on) {
  if (on) s_down[c >> 5] |= 1u << (c & 31);
  else s_down[c >> 5] &= ~(1u << (c & 31));
}

static bool isModifier(int c) {
  return c == KEY_SHIFT_L || c == KEY_SHIFT_R || c == KEY_CTRL || c == KEY_ALT || c == KEY_SYM ||
         c == KEY_CAPS;
}

int kbd_mods(void) {
  return (isDown(KEY_SHIFT_L) || isDown(KEY_SHIFT_R) ? MOD_SHIFT : 0) |
         (isDown(KEY_CTRL) ? MOD_CTRL : 0) | (isDown(KEY_ALT) ? MOD_ALT : 0) |
         (isDown(KEY_SYM) ? MOD_SYM : 0);
}

// Only cursor keys, Backspace/Del and printable keys repeat; Enter/Esc/F-keys don't.
static bool repeats(int c) {
  return (c >= 0x20 && c < 0x7F) || c == KEY_BACKSPACE || c == KEY_DEL || c == KEY_LEFT ||
         c == KEY_RIGHT || c == KEY_UP || c == KEY_DOWN;
}

static void pushEvent(uint8_t code, uint8_t state) {
  if (s_evLen == RING) { s_evHead = (uint8_t)((s_evHead + 1) % RING); s_evLen--; }
  key_event_t &e = s_ev[(s_evHead + s_evLen++) % RING];
  e.code = code;
  e.state = state;
  e.mods = (uint8_t)kbd_mods();
  e._pad = 0;
}

static void pushPress(uint8_t code) {
  if (s_prLen == RING) { s_prHead = (uint8_t)((s_prHead + 1) % RING); s_prLen--; }
  s_press[(s_prHead + s_prLen++) % RING] = code;
}

void sdk_kbd_pump() {
  HalKey k;
  while (hal_key_poll(&k)) {
    if (k.code == HAL_KEY_RELEASE_ALL) {
      memset(s_down, 0, sizeof s_down);
      s_repeatKey = KEY_NONE;
      continue;
    }
    if (k.code == KEY_BREAK && k.state == HAL_KEY_PRESSED) sdk_exit();
    if (k.state == HAL_KEY_PRESSED) {
      setDown(k.code, true);
      pushEvent(k.code, KEY_PRESSED);
      if (!isModifier(k.code)) pushPress(k.code);
      s_repeatKey = repeats(k.code) ? k.code : KEY_NONE;
      s_repeatAt = hal_millis() + REPEAT_DELAY_MS;
    } else if (k.state == HAL_KEY_HELD) {
      pushEvent(k.code, KEY_HELD);
    } else if (k.state == HAL_KEY_RELEASED) {
      setDown(k.code, false);
      pushEvent(k.code, KEY_RELEASED);
      if (k.code == s_repeatKey) s_repeatKey = KEY_NONE;
    }
  }
}

void sdk_kbd_reset() {
  // Drop pending keys without acting on them: a Break left over from a stopped program must not
  // call sdk_exit() from here.
  HalKey k;
  while (hal_key_poll(&k)) {}
  memset(s_down, 0, sizeof s_down);
  s_evLen = s_prLen = 0;
  s_repeatKey = KEY_NONE;
}

int kbd_get(void) {
  sdk_kbd_pump();
  if (s_prLen) {
    uint8_t c = s_press[s_prHead];
    s_prHead = (uint8_t)((s_prHead + 1) % RING);
    s_prLen--;
    return c;
  }
  if (s_repeatKey != KEY_NONE && isDown(s_repeatKey) && (int32_t)(hal_millis() - s_repeatAt) >= 0) {
    s_repeatAt = hal_millis() + REPEAT_PERIOD_MS;
    return s_repeatKey;
  }
  return KEY_NONE;
}

int kbd_wait(void) {
  for (;;) {
    int k = kbd_get();
    if (k != KEY_NONE) return k;
    hal_delay(5);
  }
}

bool kbd_event(key_event_t *ev) {
  sdk_kbd_pump();
  if (!s_evLen) return false;
  *ev = s_ev[s_evHead];
  s_evHead = (uint8_t)((s_evHead + 1) % RING);
  s_evLen--;
  return true;
}

bool kbd_down(int code) {
  sdk_kbd_pump();
  return code > 0 && code < 256 && isDown(code);
}

void kbd_flush(void) {
  sdk_kbd_pump();
  s_evLen = s_prLen = 0;
}

void kbd_backlight(uint8_t level) { hal_kbd_backlight(level); }
