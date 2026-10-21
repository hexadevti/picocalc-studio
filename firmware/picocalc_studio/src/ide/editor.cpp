// editor.cpp - see editor.h.
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "editor.h"
#include "../ui/ui.h"

#define GUTTER_BG RGB(24, 24, 36)
#define LINE_BG   RGB(30, 30, 48)
#define ERR_BG    RGB(100, 20, 24)
#define SEL_BG    RGB(50, 80, 150)

// Indexed by HL_*.
static const color_t kColors[] = {
    RGB(220, 220, 220), RGB(110, 170, 255), RGB(80, 200, 180), RGB(190, 220, 160), RGB(230, 150, 110),
    RGB(110, 150, 90),  RGB(200, 140, 200), RGB(230, 190, 90), RGB(230, 230, 160), RGB(170, 170, 180),
};

static char *s_clip;       // clipboard, shared by all files
static int s_clipLen;
static bool s_clipLines;   // whole lines (copied without a selection): paste above the cursor line

// Keeps the cursor inside the doc and on screen.
static void fix(Doc *d, int rows) {
  if (d->cy >= d->n) d->cy = d->n - 1;
  if (d->cy < 0) d->cy = 0;
  if (d->cx > d->ln[d->cy].len) d->cx = d->ln[d->cy].len;
  if (d->cx < 0) d->cx = 0;
  if (d->ay >= d->n) { d->ay = d->n - 1; d->ax = d->ln[d->ay].len; }
  if (d->ax > d->ln[d->ay].len) d->ax = d->ln[d->ay].len;
  if (d->cy < d->top) d->top = d->cy;
  if (d->cy >= d->top + rows) d->top = d->cy - rows + 1;
  if (d->top > d->n - 1) d->top = d->n - 1;
  if (d->top < 0) d->top = 0;
  if (d->cx < d->left) d->left = d->cx > 8 ? d->cx - 8 : 0;
  if (d->cx >= d->left + ED_COLS) d->left = d->cx - ED_COLS + 9;
}

// The selection, start first. False if there is none (or it is empty).
static bool selRange(const Doc *d, int *y0, int *x0, int *y1, int *x1) {
  if (!d->sel || (d->ay == d->cy && d->ax == d->cx)) return false;
  const bool anchorFirst = d->ay < d->cy || (d->ay == d->cy && d->ax < d->cx);
  *y0 = anchorFirst ? d->ay : d->cy;
  *x0 = anchorFirst ? d->ax : d->cx;
  *y1 = anchorFirst ? d->cy : d->ay;
  *x1 = anchorFirst ? d->cx : d->ax;
  return true;
}

bool ed_has_sel(const Doc *d) {
  int a, b, c, e;
  return selRange(d, &a, &b, &c, &e);
}

void ed_draw(Doc *d, int y0, int rows, int errLine) {
  static uint8_t *cls;
  static int clsCap;
  fix(d, rows);
  int sy0 = -1, sx0 = 0, sy1 = -1, sx1 = 0;
  if (!selRange(d, &sy0, &sx0, &sy1, &sx1)) sy0 = sy1 = -1;
  gfx_font(FONT_6X10);
  gfx_fill_rect(0, y0, ED_X0 - 3, rows * UI_CH, GUTTER_BG);
  gfx_fill_rect(ED_X0 - 3, y0, SCREEN_W - ED_X0 + 3, rows * UI_CH, UI_BG);
  char buf[ED_COLS + 1];
  for (int r = 0; r < rows; r++) {
    const int i = d->top + r, y = y0 + r * UI_CH;
    if (i >= d->n) break;
    const DocLine *l = &d->ln[i];
    if (i == errLine || i == d->cy)
      gfx_fill_rect(ED_X0 - 3, y, SCREEN_W - ED_X0 + 3, UI_CH, i == errLine ? ERR_BG : LINE_BG);
    if (i >= sy0 && i <= sy1) {   // selected part, plus a cell for the newline
      int a = i == sy0 ? sx0 : 0, b = i == sy1 ? sx1 : l->len + 1;
      if (a < d->left) a = d->left;
      if (b > d->left + ED_COLS) b = d->left + ED_COLS;
      if (b > a) gfx_fill_rect(ED_X0 + (a - d->left) * UI_CW, y, (b - a) * UI_CW, UI_CH, SEL_BG);
    }
    snprintf(buf, sizeof buf, "%4d", i + 1);
    gfx_text(0, y, buf, i == d->cy ? RGB(230, 210, 120) : UI_DIM);
    if (l->len > clsCap) {
      int cap = l->len + 64;
      uint8_t *p = (uint8_t *)realloc(cls, cap);
      if (!p) continue;
      cls = p;
      clsCap = cap;
    }
    doc_highlight(d, i, cls);
    const int end = l->len < d->left + ED_COLS ? l->len : d->left + ED_COLS;
    for (int x = d->left; x < end;) {
      const uint8_t c = cls[x];
      int n = 0;
      while (x + n < end && cls[x + n] == c) { buf[n] = l->s[x + n]; n++; }
      buf[n] = 0;
      gfx_text(ED_X0 + (x - d->left) * UI_CW, y, buf, kColors[c]);
      x += n;
    }
  }
  // cursor: an inverted cell, orange in mark mode
  const int cr = d->cy - d->top;
  if (cr >= 0 && cr < rows) {
    const DocLine *l = &d->ln[d->cy];
    char ch[2] = {d->cx < l->len ? l->s[d->cx] : ' ', 0};
    gfx_text_bg(ED_X0 + (d->cx - d->left) * UI_CW, y0 + cr * UI_CH, ch, BLACK,
                d->marking ? ORANGE : RGB(240, 240, 240));
  }
  ui_scrollbar(SCREEN_W - UI_SCROLL_W, y0, rows * UI_CH, d->n, rows, d->top);
}

static bool allSpaces(const DocLine *l, int upTo) {
  for (int i = 0; i < upTo && i < l->len; i++)
    if (l->s[i] != ' ') return false;
  return true;
}

// Inserts at the cursor and moves it to the end of the text.
static void insert(Doc *d, const char *s, int n) {
  int ey, ex;
  if (doc_insert_text(d, d->cy, d->cx, s, n, &ey, &ex)) { d->cy = ey; d->cx = ex; }
}

static void deleteSel(Doc *d) {
  int y0, x0, y1, x1;
  if (selRange(d, &y0, &x0, &y1, &x1)) {
    doc_delete_range(d, y0, x0, y1, x1);
    d->cy = y0;
    d->cx = x0;
  }
  d->sel = false;
}

// Enter: keep the indent, add a level after '{', and put a '}' that follows the cursor on its own
// line below.
static void newline(Doc *d) {
  deleteSel(d);
  const DocLine *l = &d->ln[d->cy];
  int base = doc_indent(d, d->cy);
  if (base > d->cx) base = d->cx;
  int p = d->cx - 1;
  while (p >= 0 && l->s[p] == ' ') p--;
  const bool open = p >= 0 && l->s[p] == '{';
  int sp = 0;   // spaces after the cursor go: the new line gets its own indent
  while (d->cx + sp < l->len && l->s[d->cx + sp] == ' ') sp++;
  if (sp) doc_delete_range(d, d->cy, d->cx, d->cy, d->cx + sp);
  l = &d->ln[d->cy];
  const bool brace = open && d->cx < l->len && l->s[d->cx] == '}';
  const int indent = base + (open ? DOC_TAB : 0);
  char buf[2 + 2 * 128];
  int n = 0;
  buf[n++] = '\n';
  for (int i = 0; i < indent && i < 128; i++) buf[n++] = ' ';
  const int cx = n - 1;
  if (brace) {
    buf[n++] = '\n';
    for (int i = 0; i < base && i < 128; i++) buf[n++] = ' ';
  }
  const int cy = d->cy + 1;
  insert(d, buf, n);
  d->cy = cy;
  d->cx = cx;
}

// Lines the selection covers (a selection ending at column 0 doesn't take that line), or the
// cursor line.
static void selLines(const Doc *d, int *y0, int *y1) {
  int x0, x1;
  if (!selRange(d, y0, &x0, y1, &x1)) { *y0 = *y1 = d->cy; return; }
  if (x1 == 0 && *y1 > *y0) (*y1)--;
}

static void selectLines(Doc *d, int y0, int y1) {
  d->sel = true;
  d->ay = y0;
  d->ax = 0;
  d->cy = y1;
  d->cx = d->ln[y1].len;
}

static void indentLines(Doc *d, bool out) {
  int y0, y1;
  selLines(d, &y0, &y1);
  const bool hadSel = ed_has_sel(d);
  int cx = d->cx;
  for (int y = y0; y <= y1; y++) {
    if (out) {
      int n = doc_indent(d, y);
      if (n > DOC_TAB) n = DOC_TAB;
      if (n) doc_delete_range(d, y, 0, y, n);
      if (y == d->cy) cx -= n;
    } else if (d->ln[y].len) {
      int ey, ex;
      doc_insert_text(d, y, 0, "    ", DOC_TAB, &ey, &ex);
      if (y == d->cy) cx += DOC_TAB;
    }
  }
  if (hadSel) selectLines(d, y0, y1);
  else d->cx = cx < 0 ? 0 : cx;
}

static void setClip(char *s, int n, bool lines) {
  free(s_clip);
  s_clip = s;
  s_clipLen = n;
  s_clipLines = lines;
}

// Ctrl-C / Ctrl-X: the selection, or the whole cursor line.
static bool copy(Doc *d, bool cut) {
  int y0, x0, y1, x1;
  if (selRange(d, &y0, &x0, &y1, &x1)) {
    int n;
    char *s = doc_get_text(d, y0, x0, y1, x1, &n);
    if (!s) return false;
    setClip(s, n, false);
    if (cut) deleteSel(d);
    else d->marking = false;
    return true;
  }
  const bool last = d->cy == d->n - 1;
  int n;
  char *s = doc_get_text(d, d->cy, 0, d->cy, d->ln[d->cy].len, &n);
  if (!s) return false;
  char *t = (char *)realloc(s, n + 2);   // with its newline
  if (!t) { free(s); return false; }
  t[n++] = '\n';
  t[n] = 0;
  setClip(t, n, true);
  if (cut) {
    if (!last) doc_delete_range(d, d->cy, 0, d->cy + 1, 0);
    else if (d->cy > 0) doc_delete_range(d, d->cy - 1, d->ln[d->cy - 1].len, d->cy, d->ln[d->cy].len);
    else doc_delete_range(d, 0, 0, 0, d->ln[0].len);
    d->cx = doc_indent(d, d->cy < d->n ? d->cy : d->n - 1);
  }
  return true;
}

static void paste(Doc *d) {
  if (!s_clip) return;
  if (s_clipLines && !ed_has_sel(d)) {   // whole lines go above the cursor line
    const int x = d->cx;
    d->cx = 0;
    insert(d, s_clip, s_clipLen);
    d->cx = x;   // the cursor stays on its line, which moved down
    return;
  }
  deleteSel(d);
  insert(d, s_clip, s_clipLen);
}

static bool isWord(char c) { return isalnum((unsigned char)c) || c == '_'; }

// Cursor movement. Returns false if `k` isn't a movement key.
static bool move(Doc *d, int k, bool ctrl, int rows) {
  const DocLine *l = &d->ln[d->cy];
  switch (k) {
    case KEY_LEFT:
      if (ctrl) {
        while (d->cx > 0 && !isWord(l->s[d->cx - 1])) d->cx--;
        while (d->cx > 0 && isWord(l->s[d->cx - 1])) d->cx--;
      } else if (d->cx > 0) d->cx--;
      else if (d->cy > 0) { d->cy--; d->cx = d->ln[d->cy].len; }
      break;
    case KEY_RIGHT:
      if (ctrl) {
        while (d->cx < l->len && isWord(l->s[d->cx])) d->cx++;
        while (d->cx < l->len && !isWord(l->s[d->cx])) d->cx++;
      } else if (d->cx < l->len) d->cx++;
      else if (d->cy < d->n - 1) { d->cy++; d->cx = 0; }
      break;
    case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: {
      const int step = k == KEY_UP ? -1 : k == KEY_DOWN ? 1 : k == KEY_PGUP ? -(rows - 1) : rows - 1;
      d->cy += step;
      if (k == KEY_PGUP || k == KEY_PGDN) d->top += step;
      if (d->cy < 0) d->cy = 0;
      if (d->cy >= d->n) d->cy = d->n - 1;
      d->cx = d->want;
      fix(d, rows);
      return true;   // keeps `want`
    }
    case KEY_HOME:
      if (ctrl) { d->cy = 0; d->cx = 0; break; }
      {
        const int ind = doc_indent(d, d->cy);   // first non-blank, then column 0
        d->cx = d->cx == ind ? 0 : ind;
      }
      break;
    case KEY_END:
      if (ctrl) d->cy = d->n - 1;
      d->cx = d->ln[d->cy].len;
      break;
    default: return false;
  }
  fix(d, rows);
  d->want = d->cx;
  return true;
}

void ed_mark(Doc *d) {
  if (d->marking) { d->marking = false; return; }
  d->marking = true;
  d->sel = true;
  d->ay = d->cy;
  d->ax = d->cx;
}

void ed_unselect(Doc *d) {
  d->sel = false;
  d->marking = false;
}

bool ed_key(Doc *d, int key, int rows) {
  // Alt selects: the keyboard doesn't report Shift reliably with the arrows. Shift is still taken.
  const bool ctrl = key & UI_CTRL, shift = key & (UI_SHIFT | UI_ALT);
  const int k = key & 0xFF;

  // movement: Alt/Shift (or mark mode) extends the selection
  if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_UP || k == KEY_DOWN || k == KEY_PGUP ||
      k == KEY_PGDN || k == KEY_HOME || k == KEY_END) {
    const bool extend = shift || d->marking;
    if (extend && !d->sel) { d->sel = true; d->ay = d->cy; d->ax = d->cx; }
    int y0, x0, y1, x1;
    if (!extend && !ctrl && selRange(d, &y0, &x0, &y1, &x1) && (k == KEY_LEFT || k == KEY_RIGHT)) {
      // Left/Right on a selection: go to its start/end
      d->cy = k == KEY_LEFT ? y0 : y1;
      d->cx = k == KEY_LEFT ? x0 : x1;
      d->sel = false;
      fix(d, rows);
      d->want = d->cx;
      return true;
    }
    if (!extend) d->sel = false;
    return move(d, k, ctrl, rows);
  }

  const DocLine *l = &d->ln[d->cy];
  if (ctrl) {
    switch (k) {
      case ' ': ed_mark(d); return true;
      case 'z': doc_undo(d); break;
      case 'y': doc_redo(d); break;
      case 'a':
        d->sel = true;
        d->ay = 0; d->ax = 0;
        d->cy = d->n - 1; d->cx = d->ln[d->cy].len;
        d->marking = false;
        break;
      case 'c': copy(d, false); break;
      case 'x': doc_begin(d); copy(d, true); d->marking = false; break;
      case 'v': doc_begin(d); paste(d); d->marking = false; break;
      case 'k': {   // delete the line(s)
        int y0, y1;
        selLines(d, &y0, &y1);
        doc_begin(d);
        if (y1 < d->n - 1) doc_delete_range(d, y0, 0, y1 + 1, 0);
        else if (y0 > 0) doc_delete_range(d, y0 - 1, d->ln[y0 - 1].len, y1, d->ln[y1].len);
        else doc_delete_range(d, 0, 0, y1, d->ln[y1].len);
        d->cy = y0;
        ed_unselect(d);
        fix(d, rows);
        d->cx = doc_indent(d, d->cy);
        break;
      }
      case 'd': {   // duplicate the line(s)
        int y0, y1, n, ey, ex;
        selLines(d, &y0, &y1);
        char *s = doc_get_text(d, y0, 0, y1, d->ln[y1].len, &n);
        if (!s) break;
        doc_begin(d);
        doc_insert_text(d, y1, d->ln[y1].len, "\n", 1, &ey, &ex);
        doc_insert_text(d, ey, ex, s, n, &ey, &ex);
        free(s);
        if (ed_has_sel(d)) selectLines(d, y1 + 1, y1 + 1 + (y1 - y0));
        else d->cy += y1 - y0 + 1;
        break;
      }
      default: return false;
    }
    fix(d, rows);
    d->want = d->cx;
    return true;
  }

  switch (k) {
    case KEY_ENTER: doc_begin(d); newline(d); break;
    case KEY_BACKSPACE:
      doc_begin(d);
      if (ed_has_sel(d)) deleteSel(d);
      else if (d->cx > 0) {
        const int n = allSpaces(l, d->cx) ? (d->cx - 1) % DOC_TAB + 1 : 1;
        doc_delete_range(d, d->cy, d->cx - n, d->cy, d->cx);
        d->cx -= n;
      } else if (d->cy > 0) {
        const int x = d->ln[d->cy - 1].len;
        doc_delete_range(d, d->cy - 1, x, d->cy, 0);
        d->cy--;
        d->cx = x;
      }
      break;
    case KEY_DEL:
      doc_begin(d);
      if (ed_has_sel(d)) deleteSel(d);
      else if (d->cx < l->len) doc_delete_range(d, d->cy, d->cx, d->cy, d->cx + 1);
      else if (d->cy < d->n - 1) doc_delete_range(d, d->cy, d->cx, d->cy + 1, 0);
      break;
    case KEY_TAB: {
      doc_begin(d);
      int y0, x0, y1, x1;
      if (shift || (selRange(d, &y0, &x0, &y1, &x1) && y1 > y0)) { indentLines(d, shift); break; }
      deleteSel(d);
      static const char sp[] = "    ";
      insert(d, sp, DOC_TAB - d->cx % DOC_TAB);
      break;
    }
    default:
      if ((key & UI_ALT) || k < 0x20 || k >= 0x7F) return false;
      if (ed_has_sel(d)) { doc_begin(d); deleteSel(d); }
      else if (k == '}' && d->cx >= DOC_TAB && allSpaces(l, d->cx)) {   // closing brace: one level out
        doc_begin(d);
        doc_delete_range(d, d->cy, d->cx - DOC_TAB, d->cy, d->cx);
        d->cx -= DOC_TAB;
      } else doc_begin_typing(d, (char)k);
      {
        const char c = (char)k;
        insert(d, &c, 1);
      }
      break;
  }
  if (!ed_has_sel(d) || k == KEY_ENTER || k == KEY_BACKSPACE || k == KEY_DEL) d->sel = false;
  d->marking = false;
  fix(d, rows);
  d->want = d->cx;
  return true;
}

void ed_goto(Doc *d, int line, int col, int rows) {
  d->cy = line;
  d->cx = col;
  ed_unselect(d);
  fix(d, rows);
  d->want = d->cx;
  d->top = d->cy - rows / 2;
  fix(d, rows);
}

static bool matchAt(const DocLine *l, int x, const char *s, int n) {
  if (x + n > l->len) return false;
  for (int i = 0; i < n; i++)
    if (tolower((unsigned char)l->s[x + i]) != tolower((unsigned char)s[i])) return false;
  return true;
}

bool ed_find(Doc *d, const char *s, int rows) {
  return ed_find_at(d, s, rows, d->cy, d->cx + (ed_has_sel(d) ? 0 : 1));
}

bool ed_find_at(Doc *d, const char *s, int rows, int y, int x) {
  const int n = (int)strlen(s);
  if (!n) return false;
  for (int pass = 0; pass <= d->n; pass++) {
    const DocLine *l = &d->ln[y];
    for (; x + n <= l->len; x++)
      if (matchAt(l, x, s, n)) {
        const bool onScreen = y >= d->top && y < d->top + rows;
        if (!onScreen) ed_goto(d, y, x, rows);
        d->marking = false;
        d->sel = true;
        d->ay = y;
        d->ax = x;
        d->cy = y;
        d->cx = x + n;
        fix(d, rows);
        d->want = d->cx;
        return true;
      }
    y = (y + 1) % d->n;
    x = 0;
  }
  return false;
}

void ed_replace_sel(Doc *d, const char *s, int rows) {
  deleteSel(d);
  insert(d, s, (int)strlen(s));
  fix(d, rows);
  d->want = d->cx;
}
