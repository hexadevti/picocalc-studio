// ui.h - the IDE's screen furniture: title and status bars, lists, text prompts, message boxes.
// Everything is drawn with FONT_6X10 (53 x 32 characters) through picocalc.h.
#pragma once

#include "../sdk/picocalc.h"

#define UI_CW 6            // character cell
#define UI_CH 10
#define UI_BAR_H 12        // title and status bars
#define UI_COLS (SCREEN_W / UI_CW)

#define UI_BG      RGB(14, 14, 22)
#define UI_BAR     RGB(20, 40, 90)
#define UI_SEL     RGB(40, 70, 140)
#define UI_TEXT    RGB(220, 220, 220)
#define UI_DIM     RGB(120, 120, 140)

// Keys: kbd_get() codes, with Ctrl and Alt as flags above them. Ctrl+letter comes back as the
// lower-case letter | UI_CTRL whichever way the keyboard reports it. Shift is only flagged on
// keys it doesn't already change (arrows, Tab, F-keys...).
#define UI_CTRL  0x100
#define UI_ALT   0x200
#define UI_SHIFT 0x400
int ui_key();              // KEY_NONE if no key is waiting
int ui_wait_key();

// A vertical scroll bar: `visible` of `total` rows shown, starting at row `top`.
#define UI_SCROLL_W 3
void ui_scrollbar(int x, int y, int h, int total, int visible, int top);

void ui_title(const char *left, const char *right);   // top bar
void ui_status(const char *left, const char *right, color_t leftColor);   // bottom bar

// A scrolling list. Returns the chosen index (Enter), -1 for Esc, or -2 for any other key that
// isn't navigation, stored in *key (if key is null those are ignored). *sel is the highlighted
// item, in and out.
int ui_list(const char *title, const char *const *items, int n, int *sel, const char *help, int *key);

// One-line text prompt in a box. buf holds the initial text. False on Esc.
bool ui_input(const char *title, const char *prompt, char *buf, int size);

// Scrollable text, wrapped at the screen width. Esc or Enter closes it.
void ui_text_view(const char *title, const char *text);

// Shows a question in a box and waits for one of `keys` (lower case) or Esc. Returns the key
// pressed, or KEY_ESC.
int ui_ask(const char *question, const char *keys);

// Shows a one-line message box until a key is pressed.
void ui_message(const char *msg);
