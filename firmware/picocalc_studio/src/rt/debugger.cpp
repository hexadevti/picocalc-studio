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
#include "../ide/doc.h"
#include "debugger.h"

#define CW 6
#define CH 10
#define COLS (SCREEN_W / CW)   // 53
#define ROWS (SCREEN_H / CH)   // 32
#define SRC_ROW 1              // source rows SRC_ROW .. SRC_ROW + SRC_ROWS - 1
#define SRC_ROWS 20
#define STK_BAR (SRC_ROW + SRC_ROWS)   // the call stack's title, then its frames
#define STK_ROW (STK_BAR + 1)
#define STK_ROWS (ROWS - 1 - STK_ROW)  // 9
#define MAX_FRAMES 32
#define GUTTER 6               // "  123 " / "* 123>"
#define TAB 4
#define LINE_MAX 192           // a source line's first LINE_MAX cells, tabs expanded
#define KEYS " F4 run  Down next  Right step  Left finish  Esc end"

#define C_BG      0x0000
#define C_TEXT    0xC618
#define C_GUTTER  0x8410
#define C_GUT_BG  0x2104
#define C_CUR_BG  0x0300
#define C_CUR     0xFFFF
#define C_BAR_BG  0x0010
#define C_BAR     0xFFFF
#define C_BP      0xF800
#define C_STK_BAR 0x2945
#define C_SEL_BG  0x0300

// n cells of s (padded with spaces) at a cell position; with cls (n entries), each cell takes the
// editor's color for its HL_* class instead of fg.
static void text(int col, int row, const char *s, int n, uint16_t fg, uint16_t bg,
                 const uint8_t *cls = nullptr) {
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
      const uint16_t f = cls ? hl_colors[cls[i + c]] : fg;
      for (int r = 0; r < CH; r++)
        for (int b = 0; b < CW; b++) px[r * k * CW + c * CW + b] = (g[r] << b) & 0x80 ? f : bg;
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

// src with its tabs expanded and the line end dropped, into t (LINE_MAX + 1 bytes); its length.
static int expand(const char *src, char *t) {
  int n = 0;
  for (const char *p = src; *p && *p != '\n' && *p != '\r' && n < LINE_MAX; p++)
    if (*p == '\t')
      do t[n++] = ' '; while (n % TAB && n < LINE_MAX);
    else
      t[n++] = *p;
  t[n] = 0;
  return n;
}

// One source row: gutter, then the line (expanded) colored by its classes.
static void sourceRow(int r, int line, const char *t, const uint8_t *cls, bool cur, bool bp) {
  char g[GUTTER + 1];
  snprintf(g, sizeof g, "%c%4d%c", bp ? '*' : ' ', line, cur ? '>' : ' ');
  text(0, r, g, 1, bp ? C_BP : C_GUTTER, C_GUT_BG);
  text(1, r, g + 1, GUTTER - 1, cur ? C_CUR : C_GUTTER, C_GUT_BG);
  text(GUTTER, r, t, COLS - GUTTER + 1, C_TEXT, cur ? C_CUR_BG : C_BG, cls);
}

static const char *baseName(const char *path) {
  const char *s = strrchr(path, '/');
  return s ? s + 1 : path;
}

// The source around `line`, with the line marked, highlighted as the editor does. Every line
// from the top is scanned, for the /* comments that span lines.
static void drawSource(const PcxDbg *d, const char *file, int line) {
  // The stop line a third of the way down.
  int first = line - SRC_ROWS / 3;
  if (first < 1) first = 1;
  FILE *f = fopen(file, "r");
  char src[160], t[LINE_MAX + 1];
  uint8_t cls[LINE_MAX + COLS];   // HL_TEXT past the line's end: text() reads COLS - GUTTER + 1
  int at = 1, r = 0;
  bool partial = false;   // src holds the start of a line longer than the buffer
  bool comment = false;   // inside a /* comment
  while (f && r < SRC_ROWS && fgets(src, sizeof src, f)) {
    const bool whole = strchr(src, '\n') != nullptr;
    const int n = expand(src, t);
    const bool show = !partial && at >= first;
    if (show) memset(cls, HL_TEXT, sizeof cls);
    comment = hl_scan(t, n, comment, show ? cls : nullptr);
    if (show) {
      sourceRow(SRC_ROW + r, at, t, cls, at == line, isBreak(d, file, at));
      r++;
    }
    partial = !whole;
    if (whole) at++;
  }
  if (f) fclose(f);
  else row(SRC_ROW + r++, "  (cannot open the source file)", C_TEXT, C_BG);
  for (; r < SRC_ROWS; r++) row(SRC_ROW + r, "", C_TEXT, C_BG);
}

// The call stack, innermost first, scrolled to show frame `sel`, which is highlighted.
static void drawStack(const PcxDbg *d, const PcxFrame *f, int nf, int sel) {
  char buf[COLS + 2];
  snprintf(buf, sizeof buf, " call stack (%d)%*s", nf, COLS - 17, "PgUp/PgDn frame ");
  row(STK_BAR, buf, C_BAR, C_STK_BAR);
  const int first = sel < STK_ROWS ? 0 : sel - STK_ROWS + 1;
  for (int r = 0; r < STK_ROWS; r++) {
    const int i = first + r;
    if (i >= nf) {
      row(STK_ROW + r, "", C_TEXT, C_BG);
      continue;
    }
    const char *file = nullptr;
    int line = 0;
    pcx_lookup_line(d->v, f[i].at, &file, &line);
    snprintf(buf, sizeof buf, "%c#%-2d %s  %s:%d", i == sel ? '>' : ' ', i,
             f[i].fn ? d->v->strtab + f[i].fn->name : "?", file ? baseName(file) : "?", line);
    row(STK_ROW + r, buf, i == sel ? C_CUR : C_TEXT, i == sel ? C_SEL_BG : C_BG);
  }
}

// Frame `sel`: its place in the title and the source, and the stack.
static void draw(const PcxDbg *d, const PcxFrame *f, int nf, int sel, const char *why) {
  char buf[COLS + 2];
  const char *file = nullptr;
  int line = 0;
  pcx_lookup_line(d->v, f[sel].at, &file, &line);
  if (!file) {
    row(0, " stopped (no line information)", C_BAR, C_BAR_BG);
    for (int r = 0; r < SRC_ROWS; r++) row(SRC_ROW + r, "", C_TEXT, C_BG);
  } else {
    if (sel) snprintf(buf, sizeof buf, " %s:%d  (frame #%d)", baseName(file), line, sel);
    else snprintf(buf, sizeof buf, " %s:%d  (%s)", baseName(file), line, why);
    row(0, buf, C_BAR, C_BAR_BG);
    drawSource(d, file, line);
  }
  drawStack(d, f, nf, sel);
  row(ROWS - 1, KEYS, C_BAR, C_BAR_BG);
}

int rt_dbg_view(const RtModule *m, const PcxDbg *d, uint32_t at, const PcxDbgRegs *regs,
                const char *why) {
  (void)m;
  PcxFrame f[MAX_FRAMES];
  int nf = pcx_dbg_backtrace(d, at, regs->x[8], f, MAX_FRAMES);
  if (nf < 1) {
    f[0] = {at, regs->x[8], nullptr};
    nf = 1;
  }
  int sel = 0;
  // Keys pressed before the stop were the program's.
  HalKey k;
  while (hal_key_poll(&k)) {}
  draw(d, f, nf, sel, why);

  int mode = -2;
  while (mode == -2) {
    if (!hal_key_poll(&k)) {
      hal_delay(10);
      continue;
    }
    if (k.state != HAL_KEY_PRESSED) continue;
    switch (k.code) {
      case KEY_PGDN:   // outward, to the caller
        if (sel + 1 < nf) draw(d, f, nf, ++sel, why);
        break;
      case KEY_PGUP:
        if (sel > 0) draw(d, f, nf, --sel, why);
        break;
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
