// debugger.cpp - the stopped view of a debug run: the source around the stop, and the keys that
// resume the program (docs/DEBUGGER.md).
//
// It draws straight to the LCD, never into the framebuffer, which is the program's: leaving puts
// the framebuffer back on the screen. Text is drawn a few cells at a time from a small buffer, and
// the source is read from the card line by line, so the view needs next to no memory (the program
// may have taken it all).
#include <stdio.h>
#include <string.h>
#include "../hal/hal.h"
#include "../sdk/picocalc.h"
#include "../sdk/uifont.h"
#include "debugger.h"

#define CW 6
#define CH 10
#define COLS (SCREEN_W / CW)   // 53
#define ROWS (SCREEN_H / CH)   // 32
#define SRC_ROW 1              // source rows SRC_ROW .. SRC_ROW + SRC_ROWS - 1
#define SRC_ROWS (ROWS - 3)
#define GUTTER 6               // "  123 " / "* 123>"
#define TAB 4

#define C_BG      0x0000
#define C_TEXT    0xC618
#define C_GUTTER  0x8410
#define C_GUT_BG  0x2104
#define C_CUR_BG  0x0300
#define C_CUR     0xFFFF
#define C_BAR_BG  0x0010
#define C_BAR     0xFFFF
#define C_BP      0xF800

// n cells of s (padded with spaces) at a cell position.
static void text(int col, int row, const char *s, int n, uint16_t fg, uint16_t bg) {
  enum { CHUNK = 8 };
  uint16_t px[CHUNK * CW * CH];
  bool end = false;
  for (int i = 0; i < n; i += CHUNK) {
    const int x = (col + i) * CW;
    const int k = n - i < CHUNK ? n - i : CHUNK;
    int w = k * CW;
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (w <= 0) return;
    for (int c = 0; c < k; c++) {
      if (!end && !s[i + c]) end = true;
      unsigned ch = end ? ' ' : (uint8_t)s[i + c];
      if (ch < 0x20 || ch > 0x7E) ch = '?';
      const uint8_t *g = &UIFONT_6X10[(ch - 0x20) * CH];
      for (int r = 0; r < CH; r++)
        for (int b = 0; b < CW; b++) px[r * k * CW + c * CW + b] = (g[r] << b) & 0x80 ? fg : bg;
    }
    hal_lcd_push(x, row * CH, w, CH, px, k * CW);
  }
}

static void row(int r, const char *s, uint16_t fg, uint16_t bg) { text(0, r, s, COLS + 1, fg, bg); }

// Whether `line` of `file` (a line table name) holds a breakpoint.
static bool isBreak(const PcxDbg *d, const char *file, int line) {
  for (uint32_t i = 0; i < d->n; i++) {
    if (!d->bp[i]) continue;
    const char *f;
    int l;
    if (pcx_lookup_line(d->v, d->v->stops[i], &f, &l) && l == line && f == file) return true;
  }
  return false;
}

// One source row: gutter, then the line with its tabs expanded.
static void sourceRow(int r, int line, const char *src, bool cur, bool bp) {
  char g[GUTTER + 1], t[COLS - GUTTER + 1];
  snprintf(g, sizeof g, "%c%4d%c", bp ? '*' : ' ', line, cur ? '>' : ' ');
  int n = 0;
  for (const char *p = src; *p && *p != '\n' && *p != '\r' && n < (int)sizeof t - 1; p++)
    if (*p == '\t')
      do t[n++] = ' '; while (n % TAB && n < (int)sizeof t - 1);
    else
      t[n++] = *p;
  t[n] = 0;
  text(0, r, g, 1, bp ? C_BP : C_GUTTER, C_GUT_BG);
  text(1, r, g + 1, GUTTER - 1, cur ? C_CUR : C_GUTTER, C_GUT_BG);
  text(GUTTER, r, t, COLS - GUTTER + 1, cur ? C_CUR : C_TEXT, cur ? C_CUR_BG : C_BG);
}

static void draw(const PcxDbg *d, const char *file, int line, const char *why) {
  char buf[COLS + 2];
  const char *name = strrchr(file, '/');
  snprintf(buf, sizeof buf, " %s:%d  (%s)", name ? name + 1 : file, line, why);
  row(0, buf, C_BAR, C_BAR_BG);

  // The stop line a third of the way down.
  int first = line - SRC_ROWS / 3;
  if (first < 1) first = 1;
  FILE *f = fopen(file, "r");
  char src[160];
  int at = 1, r = 0;
  bool partial = false;   // src holds the start of a line longer than the buffer
  while (f && r < SRC_ROWS && fgets(src, sizeof src, f)) {
    const bool whole = strchr(src, '\n') != nullptr;
    if (!partial && at >= first) {
      sourceRow(SRC_ROW + r, at, src, at == line, isBreak(d, file, at));
      r++;
    }
    partial = !whole;
    if (whole) at++;
  }
  if (f) fclose(f);
  else row(SRC_ROW + r++, "  (cannot open the source file)", C_TEXT, C_BG);
  for (; r < SRC_ROWS; r++) row(SRC_ROW + r, "", C_TEXT, C_BG);
  row(ROWS - 2, "", C_TEXT, C_BG);
  row(ROWS - 1, " F4 run  Down next  Right step  Left finish  Esc end", C_BAR, C_BAR_BG);
}

int rt_dbg_view(const RtModule *m, const PcxDbg *d, int id, const PcxDbgRegs *regs, const char *why) {
  (void)regs;
  const char *file = nullptr;
  int line = 0;
  pcx_lookup_line(&m->view, m->view.stops[id], &file, &line);
  // Keys pressed before the stop were the program's.
  HalKey k;
  while (hal_key_poll(&k)) {}
  if (file) draw(d, file, line, why);
  else {
    row(0, " stopped (no line information)", C_BAR, C_BAR_BG);
    row(ROWS - 1, " F4 run  Down next  Right step  Left finish  Esc end", C_BAR, C_BAR_BG);
  }

  int mode = -2;
  while (mode == -2) {
    if (!hal_key_poll(&k)) {
      hal_delay(10);
      continue;
    }
    if (k.state != HAL_KEY_PRESSED) continue;
    switch (k.code) {
      case KEY_F4: case KEY_ENTER: case 'c': mode = PCX_DBG_RUN; break;
      case KEY_DOWN: case 'n': mode = PCX_DBG_STEP_OVER; break;
      case KEY_RIGHT: case 's': mode = PCX_DBG_STEP_INTO; break;
      case KEY_LEFT: case 'f': mode = PCX_DBG_STEP_OUT; break;
      case KEY_ESC: case KEY_BREAK: case 'q': mode = -1; break;
    }
  }
  hal_lcd_push(0, 0, SCREEN_W, SCREEN_H, gfx_framebuffer(), SCREEN_W);
  // The program saw none of these keys: its held-key state starts over.
  while (hal_key_poll(&k)) {}
  hal_key_inject(HAL_KEY_RELEASE_ALL, HAL_KEY_RELEASED);
  return mode;
}
