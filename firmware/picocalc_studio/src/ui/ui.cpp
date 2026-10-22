// ui.cpp - see ui.h.
#include <string.h>
#include <stdio.h>
#include "ui.h"

#define ROW_H 12           // list rows

void (*ui_loader_hook)();

static int readKey();

int ui_key() {
  static bool inHook;
  const int k = readKey();
  const int base = k & 0xFF;
  if ((base == KEY_F10 || (base == KEY_F5 && (k & UI_SHIFT))) && ui_loader_hook && !inHook) {
    inHook = true;
    ui_loader_hook();   // returns only if the user cancelled
    inHook = false;
  }
  return k;
}

static int readKey() {
  int k = kbd_get();
  if (k == KEY_NONE) return KEY_NONE;
  const int mods = kbd_mods();
  if (k >= 1 && k <= 26 && k != KEY_BACKSPACE && k != KEY_TAB && k != KEY_ENTER && k != '\r')
    return ('a' + k - 1) | UI_CTRL;   // the keyboard sent a control code
  if (mods & MOD_CTRL) {
    if (k >= 'A' && k <= 'Z') k += 'a' - 'A';
    k |= UI_CTRL;
  }
  if (mods & MOD_ALT) k |= UI_ALT;
  const int base = k & 0xFF;
  if ((mods & MOD_SHIFT) && (base == KEY_TAB || base >= 0x80)) k |= UI_SHIFT;
  return k;
}

int ui_wait_key() {
  for (;;) {
    int k = ui_key();
    if (k != KEY_NONE) return k;
    sys_delay(10);
  }
}

void ui_scrollbar(int x, int y, int h, int total, int visible, int top) {
  gfx_fill_rect(x, y, UI_SCROLL_W, h, RGB(36, 36, 50));
  if (total <= visible) {
    gfx_fill_rect(x, y, UI_SCROLL_W, h, RGB(70, 70, 90));
    return;
  }
  int th = (int)((int64_t)h * visible / total);
  if (th < 8) th = 8;
  int maxTop = total - visible;
  if (top > maxTop) top = maxTop;
  if (top < 0) top = 0;
  const int ty = y + (int)((int64_t)(h - th) * top / maxTop);
  gfx_fill_rect(x, ty, UI_SCROLL_W, th, RGB(150, 150, 175));
}

void ui_title(const char *left, const char *right) {
  gfx_font(FONT_6X10);
  gfx_fill_rect(0, 0, SCREEN_W, UI_BAR_H, UI_BAR);
  gfx_text(3, 1, left, WHITE);
  if (right) gfx_text(SCREEN_W - 3 - gfx_text_width(right), 1, right, LIGHTGRAY);
}

void ui_status(const char *left, const char *right, color_t leftColor) {
  gfx_font(FONT_6X10);
  const int y = SCREEN_H - UI_BAR_H;
  gfx_fill_rect(0, y, SCREEN_W, UI_BAR_H, RGB(40, 40, 52));
  int rw = right ? gfx_text_width(right) : 0;
  if (right) gfx_text(SCREEN_W - 3 - rw, y + 1, right, LIGHTGRAY);
  if (left) {
    gfx_clip(0, y, SCREEN_W - rw - 9, UI_BAR_H);
    gfx_text(3, y + 1, left, leftColor);
    gfx_noclip();
  }
}

int ui_list(const char *title, const char *const *items, int n, int *selp, const char *help, int *key) {
  const int y0 = UI_BAR_H + 2, rows = (SCREEN_H - 2 * UI_BAR_H - 4) / ROW_H;
  int top = 0, &sel = *selp;
  if (sel < 0 || sel >= n) sel = 0;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    gfx_clear(UI_BG);
    char pos[16];
    snprintf(pos, sizeof pos, n ? "%d/%d" : "", sel + 1, n);
    ui_title(title, pos);
    for (int r = 0; r < rows && top + r < n; r++) {
      const int i = top + r, y = y0 + r * ROW_H;
      if (i == sel) gfx_fill_rect(0, y, SCREEN_W, ROW_H, UI_SEL);
      gfx_clip(0, y, SCREEN_W - 4, ROW_H);
      gfx_text(6, y + 1, items[i], i == sel ? WHITE : UI_TEXT);
      gfx_noclip();
    }
    if (!n) gfx_text(6, y0 + 1, "(empty)", UI_DIM);
    if (n > rows) ui_scrollbar(SCREEN_W - UI_SCROLL_W, y0, rows * ROW_H, n, rows, top);
    ui_status(help, nullptr, LIGHTGRAY);
    gfx_flush();
    const int k = ui_wait_key() & ~UI_SHIFT;
    switch (k) {
      case KEY_UP: sel = sel > 0 ? sel - 1 : n - 1; break;
      case KEY_DOWN: sel = sel < n - 1 ? sel + 1 : 0; break;
      case KEY_PGUP: sel = sel > rows ? sel - rows : 0; break;
      case KEY_PGDN: sel = sel + rows < n ? sel + rows : n - 1; break;
      case KEY_HOME: sel = 0; break;
      case KEY_END: sel = n - 1; break;
      case KEY_ENTER: if (n) return sel; break;
      case KEY_ESC: return -1;
      default:
        if (key) { *key = k; return -2; }
    }
    if (sel < 0) sel = 0;
  }
}

static void box(int y, int h, const char *title) {
  gfx_fill_rect(8, y, SCREEN_W - 16, h, RGB(30, 30, 46));
  gfx_rect(8, y, SCREEN_W - 16, h, LIGHTGRAY);
  if (title) {
    gfx_fill_rect(9, y + 1, SCREEN_W - 18, UI_BAR_H, UI_BAR);
    gfx_text(13, y + 2, title, WHITE);
  }
}

bool ui_input(const char *title, const char *prompt, char *buf, int size) {
  int len = (int)strlen(buf);
  const int y = 110, fieldCols = (SCREEN_W - 32) / UI_CW;
  for (;;) {
    box(y, 64, title);
    gfx_text(14, y + 18, prompt, UI_TEXT);
    gfx_fill_rect(14, y + 34, SCREEN_W - 28, 14, BLACK);
    const int first = len >= fieldCols ? len - fieldCols + 1 : 0;
    gfx_text(16, y + 36, buf + first, WHITE);
    gfx_fill_rect(16 + (len - first) * UI_CW, y + 36, UI_CW, UI_CH, LIGHTGRAY);
    gfx_text(14, y + 51, "Enter: OK   Esc: cancel", UI_DIM);
    gfx_flush_rect(8, y, SCREEN_W - 16, 64);
    const int k = ui_wait_key();
    if (k == KEY_ENTER) return true;
    if (k == KEY_ESC) return false;
    if (k == KEY_BACKSPACE) { if (len) buf[--len] = 0; }
    else if (k >= 0x20 && k < 0x7F && len < size - 1) { buf[len++] = (char)k; buf[len] = 0; }
  }
}

int ui_ask(const char *question, const char *keys) {
  for (;;) {   // redrawn after each key: a cancelled F10 question covers this one
    box(124, 36, nullptr);
    gfx_clip(10, 124, SCREEN_W - 20, 36);
    gfx_text(14, 131, question, WHITE);
    gfx_noclip();
    gfx_text(14, 145, "Esc: cancel", UI_DIM);
    gfx_flush_rect(8, 124, SCREEN_W - 16, 36);
    int k = ui_wait_key() & 0xFF;
    if (k == KEY_ESC) return KEY_ESC;
    if (k >= 'A' && k <= 'Z') k += 'a' - 'A';
    if (k && strchr(keys, k)) return k;
  }
}

void ui_message(const char *msg) {
  box(124, 36, nullptr);
  gfx_clip(10, 124, SCREEN_W - 20, 36);
  gfx_text(14, 131, msg, WHITE);
  gfx_noclip();
  gfx_text(14, 145, "Any key: continue", UI_DIM);
  gfx_flush_rect(8, 124, SCREEN_W - 16, 36);
  ui_wait_key();
}

// Next wrapped row of `p`: its length, and where the row after it starts.
static const char *wrapRow(const char *p, int cols, int *len) {
  int n = 0;
  while (p[n] && p[n] != '\n' && n < cols) n++;
  *len = n;
  return p[n] == '\n' ? p + n + 1 : p + n;
}

void ui_text_view(const char *title, const char *text) {
  const int cols = UI_COLS - 1, rows = (SCREEN_H - 2 * UI_BAR_H - 4) / UI_CH;
  int total = 0;
  for (const char *p = text; *p; total++) { int l; p = wrapRow(p, cols, &l); }
  int top = 0;
  for (;;) {
    if (top > total - rows) top = total - rows;
    if (top < 0) top = 0;
    gfx_clear(UI_BG);
    ui_title(title, nullptr);
    const char *p = text;
    int l;
    for (int i = 0; i < top && *p; i++) p = wrapRow(p, cols, &l);
    char line[UI_COLS + 1];
    for (int r = 0; r < rows && *p; r++) {
      const char *next = wrapRow(p, cols, &l);
      memcpy(line, p, l);
      line[l] = 0;
      gfx_text(3, UI_BAR_H + 2 + r * UI_CH, line, UI_TEXT);
      p = next;
    }
    ui_scrollbar(SCREEN_W - UI_SCROLL_W, UI_BAR_H + 2, rows * UI_CH, total, rows, top);
    char pos[40];
    snprintf(pos, sizeof pos, "%d-%d of %d", total ? top + 1 : 0, top + rows < total ? top + rows : total, total);
    ui_status("Up/Down/PgUp/PgDn: scroll  Esc: close", pos, LIGHTGRAY);
    gfx_flush();
    switch (ui_wait_key() & ~UI_SHIFT) {
      case KEY_UP: top--; break;
      case KEY_DOWN: top++; break;
      case KEY_PGUP: top -= rows - 1; break;
      case KEY_PGDN: case KEY_SPACE: top += rows - 1; break;
      case KEY_HOME: top = 0; break;
      case KEY_END: top = total; break;
      case KEY_ESC: case KEY_ENTER: return;
    }
  }
}
