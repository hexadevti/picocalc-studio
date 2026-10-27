// doc.cpp - see doc.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "esp_heap_caps.h"
#include "doc.h"
#include "../sdk/picocalc.h"

#define MAX_FILE (1024 * 1024)

static void *psRealloc(void *p, size_t n) {
  return heap_caps_realloc(p, n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool reserveLines(Doc *d, int n) {
  if (n <= d->cap) return true;
  int cap = d->cap ? d->cap : 64;
  while (cap < n) cap *= 2;
  DocLine *ln = (DocLine *)psRealloc(d->ln, cap * sizeof *ln);
  if (!ln) return false;
  d->ln = ln;
  uint8_t *hl = (uint8_t *)psRealloc(d->hl, cap);
  if (!hl) return false;
  d->hl = hl;
  d->cap = cap;
  return true;
}

static bool reserveChars(DocLine *l, int n) {
  if (n <= l->cap) return true;
  int cap = l->cap ? l->cap : 16;
  while (cap < n) cap *= 2;
  char *s = (char *)psRealloc(l->s, cap);
  if (!s) return false;
  l->s = s;
  l->cap = cap;
  return true;
}

static void touch(Doc *d, int y) {
  d->modified = true;
  if (d->hlValid > y + 1) d->hlValid = y + 1;
}

// Appends text to a fresh doc, splitting lines and expanding tabs.
// Makes the lines of an empty doc from text given in pieces. Each line is allocated once, at its
// length, and the line array is cut to fit at the end: a big file is opened with little room.
struct Loader {
  Doc *d;
  DocLine cur = {nullptr, 0, 0};   // the line being read

  explicit Loader(Doc *doc) : d(doc) {}
  ~Loader() { free(cur.s); }

  bool endLine() {
    if (!reserveLines(d, d->n + 1)) return false;
    DocLine &l = d->ln[d->n];
    l = {nullptr, 0, 0};
    if (cur.len) {
      if (!(l.s = (char *)psRealloc(nullptr, cur.len))) return false;
      memcpy(l.s, cur.s, cur.len);
      l.len = l.cap = cur.len;
    }
    d->n++;
    cur.len = 0;
    return true;
  }

  bool put(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
      const char c = text[i];
      if (c == '\r') continue;
      if (c == '\n') {
        if (!endLine()) return false;
        continue;
      }
      const int add = c == '\t' ? DOC_TAB - cur.len % DOC_TAB : 1;
      if (!reserveChars(&cur, cur.len + add)) return false;
      for (int k = 0; k < add; k++) cur.s[cur.len++] = c == '\t' ? ' ' : c;
    }
    return true;
  }

  // A final newline doesn't make an empty last line in the editor; doc_save() puts it back.
  bool finish() {
    if ((cur.len || !d->n) && !endLine()) return false;
    if (d->cap > d->n) {   // if a shrink fails, the old, bigger block holds n as well
      if (DocLine *ln = (DocLine *)psRealloc(d->ln, d->n * sizeof *d->ln)) d->ln = ln;
      if (uint8_t *hl = (uint8_t *)psRealloc(d->hl, d->n)) d->hl = hl;
      d->cap = d->n;
    }
    return true;
  }
};

static bool load(Doc *d, const char *text, size_t len) {
  Loader l(d);
  if (l.put(text, len) && l.finish()) return true;
  errno = ENOMEM;
  return false;
}

// Reads the file into an empty doc, in pieces. False with errno set on failure.
static bool loadFile(Doc *d, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  bool ok = true;
  if (fseek(f, 0, SEEK_END) || ftell(f) > MAX_FILE) { ok = false; errno = EFBIG; }
  fseek(f, 0, SEEK_SET);
  Loader l(d);
  char buf[512];
  for (size_t n; ok && (n = fread(buf, 1, sizeof buf, f)) > 0;)
    if (!l.put(buf, n)) { ok = false; errno = ENOMEM; }
  if (ok && ferror(f)) { ok = false; errno = EIO; }
  fclose(f);
  if (ok && !l.finish()) { ok = false; errno = ENOMEM; }
  return ok;
}

static Doc *s_open;            // open docs, for doc_park_all()

static void addOpen(Doc *d) {
  d->nextOpen = s_open;
  s_open = d;
}

Doc *doc_new(const char *path, const char *text) {
  Doc *d = (Doc *)calloc(1, sizeof *d);
  if (!d) return nullptr;
  snprintf(d->path, sizeof d->path, "%s", path);
  if (!load(d, text ? text : "", text ? strlen(text) : 0)) { doc_free(d); return nullptr; }
  addOpen(d);
  return d;
}

// The whole file, malloc'ed and NUL-terminated (*len gets its length). Null if it can't be read.
Doc *doc_open(const char *path) {
  Doc *d = (Doc *)calloc(1, sizeof *d);
  if (!d) { errno = ENOMEM; return nullptr; }
  snprintf(d->path, sizeof d->path, "%s", path);
  if (!loadFile(d, path)) {
    const int err = errno;
    doc_free(d);
    errno = err;
    return nullptr;
  }
  addOpen(d);
  return d;
}

// Writes to <path>.tmp first and then replaces the file, so a failed write keeps the old one.
bool doc_save(Doc *d) {
  char tmp[sizeof d->path + 4];
  snprintf(tmp, sizeof tmp, "%s.tmp", d->path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return false;
  bool ok = true;
  for (int i = 0; i < d->n && ok; i++) {
    if (d->ln[i].len && fwrite(d->ln[i].s, 1, d->ln[i].len, f) != (size_t)d->ln[i].len) ok = false;
    if (ok && (i < d->n - 1 || d->ln[i].len) && fputc('\n', f) == EOF) ok = false;
  }
  if (fclose(f)) ok = false;
  if (!ok) { remove(tmp); return false; }
  remove(d->path);                  // FAT rename() won't replace a file
  if (rename(tmp, d->path)) return false;
  d->modified = false;
  return true;
}

static void dropUndo(Doc *d, int from, int to) {
  for (int i = from; i < to; i++) free(d->u[i].s);
}

static void freeLines(Doc *d) {
  for (int i = 0; i < d->n; i++) free(d->ln[i].s);
  free(d->ln);
  free(d->hl);
  d->ln = nullptr;
  d->hl = nullptr;
  d->n = d->cap = d->hlValid = 0;
}

void doc_free(Doc *d) {
  if (!d) return;
  for (Doc **p = &s_open; *p; p = &(*p)->nextOpen)
    if (*p == d) { *p = d->nextOpen; break; }
  freeLines(d);
  dropUndo(d, 0, d->nt);
  free(d->u);
  free(d);
}

// Whether reading the file again gives the doc back: it was opened or saved and not edited since.
// (A new doc is marked modified until it is saved.)
static bool matchesFile(const Doc *d) {
  if (d->modified) return false;
  FILE *f = fopen(d->path, "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

void doc_park_all() {
  for (Doc *d = s_open; d; d = d->nextOpen) {
    if (d->parked || !matchesFile(d)) continue;
    freeLines(d);
    d->parked = true;
  }
}

void doc_unpark_all() {
  for (Doc *d = s_open; d; d = d->nextOpen) {
    if (!d->parked) continue;
    d->parked = false;
    if (!loadFile(d, d->path)) {
      // Keep the doc (the editor holds it) but never let it be saved over the file.
      freeLines(d);
      dropUndo(d, 0, d->nt);
      d->nu = d->nt = 0;
      d->readOnly = true;
      d->modified = false;
      d->cx = d->cy = d->want = d->top = d->left = 0;
      d->sel = d->marking = false;
      static const char note[] = "// Out of memory reading this file back after the build.\n// Close it and open it again.";
      if (!load(d, note, sizeof note - 1)) {
        freeLines(d);
        load(d, "", 0);           // one empty line, if even that fits
      }
      continue;
    }
    if (d->cy >= d->n) d->cy = d->n - 1;
    if (d->ay >= d->n) d->sel = false;
    if (d->top >= d->n) d->top = d->n - 1;
  }
}

// ---- breakpoints ----
// They follow their lines: inserting nl lines at (y, x) moves those below y down, and y's own when
// x is 0 (the line itself moves); deleting lines y0+1..y1 moves theirs onto y0.

static void breaksInsert(Doc *d, int y, int x, int nl) {
  for (int i = 0; i < d->nbp; i++)
    if (d->bp[i] > y || (d->bp[i] == y && x == 0)) d->bp[i] += nl;
}

static void breaksDelete(Doc *d, int y0, int y1) {
  int k = 0;
  for (int i = 0; i < d->nbp; i++) {
    const int b = d->bp[i] > y1 ? d->bp[i] - (y1 - y0) : d->bp[i] > y0 ? y0 : d->bp[i];
    bool dup = false;
    for (int j = 0; j < k; j++) dup |= d->bp[j] == b;
    if (!dup) d->bp[k++] = b;
  }
  d->nbp = k;
}

bool doc_has_break(const Doc *d, int y) {
  for (int i = 0; i < d->nbp; i++)
    if (d->bp[i] == y) return true;
  return false;
}

bool doc_toggle_break(Doc *d, int y) {
  for (int i = 0; i < d->nbp; i++)
    if (d->bp[i] == y) {
      d->bp[i] = d->bp[--d->nbp];
      return true;
    }
  if (d->nbp == DOC_MAX_BREAKS) return false;
  d->bp[d->nbp++] = y;
  return true;
}

// ---- raw edits (not recorded) ----

static bool rawInsert(Doc *d, int y, int x, const char *s, int n, int *ey, int *ex) {
  if (x > d->ln[y].len) x = d->ln[y].len;
  touch(d, y);
  // the part of line y after x moves to the end of the inserted text
  int nl = 0;
  for (int i = 0; i < n; i++) nl += s[i] == '\n';
  if (!reserveLines(d, d->n + nl)) return false;
  DocLine tail = {nullptr, 0, 0};
  DocLine *l = &d->ln[y];
  if (nl) {
    if (!reserveChars(&tail, l->len - x)) return false;
    memcpy(tail.s, l->s + x, l->len - x);
    tail.len = l->len - x;
    l->len = x;
    memmove(&d->ln[y + 1 + nl], &d->ln[y + 1], (d->n - y - 1) * sizeof *d->ln);
    memset(&d->ln[y + 1], 0, nl * sizeof *d->ln);
    d->n += nl;
    breaksInsert(d, y, x, nl);
  }
  int cy = y, cx = x;
  for (int i = 0; i < n;) {
    int k = 0;
    while (i + k < n && s[i + k] != '\n') k++;
    DocLine *c = &d->ln[cy];
    if (k) {
      if (!reserveChars(c, c->len + k)) { free(tail.s); return false; }
      memmove(c->s + cx + k, c->s + cx, c->len - cx);
      memcpy(c->s + cx, s + i, k);
      c->len += k;
      cx += k;
    }
    i += k;
    if (i < n) { cy++; cx = 0; i++; }   // the newline
  }
  if (nl) {
    DocLine *c = &d->ln[cy];
    if (reserveChars(c, c->len + tail.len)) {
      memcpy(c->s + c->len, tail.s, tail.len);
      c->len += tail.len;
    }
    free(tail.s);
  }
  *ey = cy;
  *ex = cx;
  return true;
}

static void rawDelete(Doc *d, int y0, int x0, int y1, int x1) {
  if (x0 > d->ln[y0].len) x0 = d->ln[y0].len;
  if (x1 > d->ln[y1].len) x1 = d->ln[y1].len;
  touch(d, y0);
  DocLine *a = &d->ln[y0], *b = &d->ln[y1];
  if (y0 == y1) {
    memmove(a->s + x0, a->s + x1, a->len - x1);
    a->len -= x1 - x0;
    return;
  }
  const int rest = b->len - x1;
  if (!reserveChars(a, x0 + rest)) return;
  b = &d->ln[y1];
  memcpy(a->s + x0, b->s + x1, rest);
  a->len = x0 + rest;
  for (int i = y0 + 1; i <= y1; i++) free(d->ln[i].s);
  memmove(&d->ln[y0 + 1], &d->ln[y1 + 1], (d->n - y1 - 1) * sizeof *d->ln);
  d->n -= y1 - y0;
  breaksDelete(d, y0, y1);
}

char *doc_get_text(const Doc *d, int y0, int x0, int y1, int x1, int *n) {
  if (x0 > d->ln[y0].len) x0 = d->ln[y0].len;
  if (x1 > d->ln[y1].len) x1 = d->ln[y1].len;
  int len = 0;
  for (int y = y0; y <= y1; y++) {
    const int a = y == y0 ? x0 : 0, b = y == y1 ? x1 : d->ln[y].len;
    len += b - a + (y < y1);
  }
  char *s = (char *)psRealloc(nullptr, len + 1);
  if (!s) return nullptr;
  char *p = s;
  for (int y = y0; y <= y1; y++) {
    const int a = y == y0 ? x0 : 0, b = y == y1 ? x1 : d->ln[y].len;
    memcpy(p, d->ln[y].s + a, b - a);
    p += b - a;
    if (y < y1) *p++ = '\n';
  }
  *p = 0;
  if (n) *n = len;
  return s;
}

// ---- undo ----

#define UNDO_MAX 2000      // records; the oldest quarter goes when full

void doc_begin(Doc *d) { d->group++; }

void doc_begin_typing(Doc *d, char c) {
  if (d->nu && d->nu == d->nt) {
    const DocUndo *r = &d->u[d->nu - 1];
    if (r->group == d->group && r->typing && r->y == d->cy && r->x + r->n == d->cx && r->n < 40 &&
        !(c == ' ' && r->s[r->n - 1] != ' '))
      return;   // keeps typing into the same group
  }
  d->group++;
}

static DocUndo *record(Doc *d, bool ins, int y, int x, char *s, int n) {
  dropUndo(d, d->nu, d->nt);
  d->nt = d->nu;
  if (d->nt == UNDO_MAX) {
    int drop = UNDO_MAX / 4;
    while (drop < d->nt && d->u[drop].group == d->u[drop - 1].group) drop++;
    dropUndo(d, 0, drop);
    memmove(d->u, d->u + drop, (d->nt - drop) * sizeof *d->u);
    d->nt -= drop;
    d->nu = d->nt;
  }
  if (d->nt == d->ucap) {
    const int cap = d->ucap ? d->ucap * 2 : 64;
    DocUndo *u = (DocUndo *)psRealloc(d->u, cap * sizeof *u);
    if (!u) { free(s); return nullptr; }
    d->u = u;
    d->ucap = cap;
  }
  // the cursor before the group: the one in its first record, else now
  int cy = d->cy, cx = d->cx;
  if (d->nt && d->u[d->nt - 1].group == d->group) { cy = d->u[d->nt - 1].cy; cx = d->u[d->nt - 1].cx; }
  DocUndo *r = &d->u[d->nt++];
  *r = {s, n, y, x, d->group, cy, cx, ins, false};
  d->nu = d->nt;
  return r;
}

bool doc_insert_text(Doc *d, int y, int x, const char *s, int n, int *ey, int *ex) {
  if (x > d->ln[y].len) x = d->ln[y].len;
  if (!n) { *ey = y; *ex = x; return true; }
  // typing: append to the previous record
  if (n == 1 && s[0] != '\n' && d->nu && d->nu == d->nt) {
    DocUndo *r = &d->u[d->nu - 1];
    if (r->group == d->group && r->typing && r->y == y && r->x + r->n == x) {
      char *t = (char *)psRealloc(r->s, r->n + 2);
      if (t) {
        if (!rawInsert(d, y, x, s, n, ey, ex)) return false;
        r->s = t;
        r->s[r->n++] = s[0];
        r->s[r->n] = 0;
        return true;
      }
    }
  }
  if (!rawInsert(d, y, x, s, n, ey, ex)) return false;
  char *copy = (char *)psRealloc(nullptr, n + 1);
  if (copy) {
    memcpy(copy, s, n);
    copy[n] = 0;
    if (DocUndo *r = record(d, true, y, x, copy, n)) r->typing = n == 1 && s[0] != '\n';
  }
  return true;
}

void doc_delete_range(Doc *d, int y0, int x0, int y1, int x1) {
  if (y0 == y1 && x0 >= x1) return;
  int n;
  char *s = doc_get_text(d, y0, x0, y1, x1, &n);
  rawDelete(d, y0, x0, y1, x1);
  if (s) record(d, false, y0, x0 < d->ln[y0].len ? x0 : d->ln[y0].len, s, n);
}

// End of `n` chars of text inserted at (y, x).
static void endOf(const DocUndo *r, int *ey, int *ex) {
  int y = r->y, x = r->x;
  for (int i = 0; i < r->n; i++)
    if (r->s[i] == '\n') { y++; x = 0; }
    else x++;
  *ey = y;
  *ex = x;
}

bool doc_undo(Doc *d) {
  if (!d->nu) return false;
  const int g = d->u[d->nu - 1].group;
  while (d->nu && d->u[d->nu - 1].group == g) {
    const DocUndo *r = &d->u[--d->nu];
    int ey, ex;
    if (r->ins) { endOf(r, &ey, &ex); rawDelete(d, r->y, r->x, ey, ex); }
    else rawInsert(d, r->y, r->x, r->s, r->n, &ey, &ex);
    d->cy = r->cy;
    d->cx = r->cx;
  }
  d->group++;   // nothing typed afterwards joins a group from before the undo
  d->sel = false;
  return true;
}

bool doc_redo(Doc *d) {
  if (d->nu == d->nt) return false;
  const int g = d->u[d->nu].group;
  while (d->nu < d->nt && d->u[d->nu].group == g) {
    const DocUndo *r = &d->u[d->nu++];
    int ey, ex;
    if (r->ins) { rawInsert(d, r->y, r->x, r->s, r->n, &ey, &ex); d->cy = ey; d->cx = ex; }
    else { endOf(r, &ey, &ex); rawDelete(d, r->y, r->x, ey, ex); d->cy = r->y; d->cx = r->x; }
  }
  d->group++;
  d->sel = false;
  return true;
}

int doc_indent(const Doc *d, int y) {
  const DocLine *l = &d->ln[y];
  int i = 0;
  while (i < l->len && l->s[i] == ' ') i++;
  return i;
}

// ---- highlighting ----

static const char *const kKeywords[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return",
    "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "extern", "const", "volatile",
    "inline", "register", "restrict", "auto", "_Alignof", "_Static_assert", "_Noreturn", "true",
    "false", "NULL", nullptr};
static const char *const kTypes[] = {
    "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "bool", "_Bool",
    "FILE", "DIR", nullptr};

static bool inList(const char *const *list, const char *s, int n) {
  for (; *list; list++)
    if ((int)strlen(*list) == n && !memcmp(*list, s, n)) return true;
  return false;
}

static inline bool identChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

// Indexed by HL_*.
const uint16_t hl_colors[] = {
    RGB(220, 220, 220), RGB(110, 170, 255), RGB(80, 200, 180), RGB(190, 220, 160), RGB(230, 150, 110),
    RGB(110, 150, 90),  RGB(200, 140, 200), RGB(230, 190, 90), RGB(230, 230, 160), RGB(170, 170, 180),
};

bool hl_scan(const char *s, int len, bool inComment, uint8_t *cls) {
  int i = 0;
  auto mark = [&](int from, int to, uint8_t c) {
    if (cls) memset(cls + from, c, to - from);
  };
  int first = 0;
  while (first < len && s[first] == ' ') first++;
  const bool preproc = !inComment && first < len && s[first] == '#';
  while (i < len) {
    const int start = i;
    if (inComment) {
      while (i < len && !(s[i] == '*' && i + 1 < len && s[i + 1] == '/')) i++;
      if (i < len) { i += 2; inComment = false; }
      mark(start, i, HL_COMMENT);
      continue;
    }
    const char c = s[i];
    if (c == '/' && i + 1 < len && s[i + 1] == '/') { mark(i, len, HL_COMMENT); return false; }
    if (c == '/' && i + 1 < len && s[i + 1] == '*') { inComment = true; i += 2; mark(start, i, HL_COMMENT); continue; }
    if (c == '"' || c == '\'' || (preproc && c == '<' && start > first)) {
      const char end = c == '<' ? '>' : c;
      for (i++; i < len && s[i] != end; i++)
        if (s[i] == '\\') i++;
      if (i < len) i++;
      if (i > len) i = len;
      mark(start, i, HL_STRING);
      continue;
    }
    if (preproc) {
      i++;
      while (i < len && s[i] != '"' && s[i] != '<' && s[i] != '/' && s[i] != '\'') i++;
      mark(start, i, HL_PREPROC);
      if (i < len && s[i] == '/' && !(i + 1 < len && (s[i + 1] == '/' || s[i + 1] == '*'))) {
        mark(i, i + 1, HL_PREPROC);
        i++;
      }
      continue;
    }
    if (isdigit((unsigned char)c) || (c == '.' && i + 1 < len && isdigit((unsigned char)s[i + 1]))) {
      while (i < len && (identChar(s[i]) || s[i] == '.')) i++;
      mark(start, i, HL_NUMBER);
      continue;
    }
    if (identChar(c)) {
      while (i < len && identChar(s[i])) i++;
      const int n = i - start;
      uint8_t k = HL_TEXT;
      if (inList(kKeywords, s + start, n)) k = HL_KEYWORD;
      else if (inList(kTypes, s + start, n) || (n > 2 && s[i - 2] == '_' && s[i - 1] == 't')) k = HL_TYPE;
      else {
        bool upper = n > 1;
        for (int j = start; j < i && upper; j++) upper = !islower((unsigned char)s[j]);
        int j = i;
        while (j < len && s[j] == ' ') j++;
        if (upper) k = HL_CONST;
        else if (j < len && s[j] == '(') k = HL_FUNC;
      }
      mark(start, i, k);
      continue;
    }
    i++;
    mark(start, i, c == ' ' ? HL_TEXT : HL_PUNCT);
  }
  return inComment;
}

void doc_highlight(Doc *d, int y, uint8_t *cls) {
  if (d->hlValid < 1) { d->hl[0] = 0; d->hlValid = 1; }
  while (d->hlValid <= y) {
    const int i = d->hlValid - 1;
    d->hl[i + 1] = hl_scan(d->ln[i].s, d->ln[i].len, d->hl[i], nullptr);
    d->hlValid++;
  }
  hl_scan(d->ln[y].s, d->ln[y].len, d->hl[y], cls);
}
