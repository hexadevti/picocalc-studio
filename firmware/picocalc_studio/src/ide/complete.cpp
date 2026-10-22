// complete.cpp - see complete.h. Names come from three tables: the headers (with the C keywords),
// read once; the project's files as saved, read again after ac_project(); and the open files,
// read again for each list or hint. Declarations (functions, macros, typedefs, struct/union/enum
// tags, enum constants, globals) are found by a scanner that knows just enough C: brace, paren
// and bracket depth and where a file-scope statement ends. In the open files every other name
// counts too, as a plain word.
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "complete.h"
#include "editor.h"
#include "../ui/ui.h"

#define LIST_ROWS 8
#define LIST_COLS 30       // longer names are cut
#define SIG_MAX   120
#define NAME_MAX  63
#define MAX_FILES 100      // project files read from disk
#define ARENA_BLK 8192

enum { K_WORD, K_KEYWORD, K_TYPE, K_FUNC, K_MACRO, K_VAR };

// Indexed by K_*: the editor's colours for the same kinds of name.
static const color_t kKindColors[] = {
    RGB(170, 170, 180), RGB(110, 170, 255), RGB(80, 200, 180), RGB(230, 230, 160), RGB(230, 190, 90),
    RGB(220, 220, 220),
};

static const struct { const char *s; uint8_t kind; } kKeywords[] = {
    {"auto", K_KEYWORD},     {"break", K_KEYWORD},    {"case", K_KEYWORD},
    {"char", K_TYPE},        {"const", K_KEYWORD},    {"continue", K_KEYWORD},
    {"default", K_KEYWORD},  {"do", K_KEYWORD},       {"double", K_TYPE},
    {"else", K_KEYWORD},     {"enum", K_KEYWORD},     {"extern", K_KEYWORD},
    {"float", K_TYPE},       {"for", K_KEYWORD},      {"goto", K_KEYWORD},
    {"if", K_KEYWORD},       {"inline", K_KEYWORD},   {"int", K_TYPE},
    {"long", K_TYPE},        {"register", K_KEYWORD}, {"restrict", K_KEYWORD},
    {"return", K_KEYWORD},   {"short", K_TYPE},       {"signed", K_TYPE},
    {"sizeof", K_KEYWORD},   {"static", K_KEYWORD},   {"struct", K_KEYWORD},
    {"switch", K_KEYWORD},   {"typedef", K_KEYWORD},  {"union", K_KEYWORD},
    {"unsigned", K_TYPE},    {"void", K_TYPE},        {"volatile", K_KEYWORD},
    {"while", K_KEYWORD},    {"_Bool", K_TYPE},       {"_Noreturn", K_KEYWORD},
    {"_Alignof", K_KEYWORD}, {"_Alignas", K_KEYWORD}, {"_Static_assert", K_KEYWORD},
};
#define N_KEYWORDS (int)(sizeof kKeywords / sizeof *kKeywords)

static void *psRealloc(void *p, size_t n) {
  return heap_caps_realloc(p, n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool identChar(int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static bool isDigit(int c) { return c >= '0' && c <= '9'; }

static int keyword(const char *s, int n) {
  for (int i = 0; i < N_KEYWORDS; i++)
    if (!strncmp(kKeywords[i].s, s, n) && !kKeywords[i].s[n]) return i;
  return -1;
}

static bool is(const char *s, int n, const char *word) { return (int)strlen(word) == n && !memcmp(s, word, n); }

static bool contains(const char *s, int n, const char *word) {
  const int m = (int)strlen(word);
  for (int i = 0; i + m <= n; i++)
    if (!memcmp(s + i, word, m)) return true;
  return false;
}

// ---- name tables ----

// Strings in blocks that are kept when the arena is reset, for the table rebuilt on every list.
struct Arena {
  char **blk;
  int nblk, cur, used;     // block cur (-1: none yet) is filled up to `used`
};

static const char *arenaStr(Arena *a, const char *s, int n) {
  if (n >= ARENA_BLK) n = ARENA_BLK - 1;
  if (a->cur < 0 || a->used + n + 1 > ARENA_BLK) {
    if (a->cur + 1 == a->nblk) {
      char **b = (char **)psRealloc(a->blk, (a->nblk + 1) * sizeof *b);
      if (!b) return nullptr;
      a->blk = b;
      if (!(b[a->nblk] = (char *)psRealloc(nullptr, ARENA_BLK))) return nullptr;
      a->nblk++;
    }
    a->cur++;
    a->used = 0;
  }
  char *p = a->blk[a->cur] + a->used;
  memcpy(p, s, n);
  p[n] = 0;
  a->used += n + 1;
  return p;
}

struct Sym {
  const char *name, *sig;  // sig: the declaration, or null
  uint8_t kind;
  int8_t file;             // project table: index into s_files, else -1
};

struct Table {
  Sym *v;
  int n, cap;
  int *hash;               // open addressing into v, -1 empty; hcap is a power of two > 2n
  int hcap;
  Arena str;
};

static uint32_t hashName(const char *s, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
  return h;
}

static int find(const Table *t, const char *s, int n) {
  if (!t->hcap) return -1;
  for (uint32_t h = hashName(s, n) & (t->hcap - 1);; h = (h + 1) & (t->hcap - 1)) {
    const int i = t->hash[h];
    if (i < 0) return -1;
    if (!strncmp(t->v[i].name, s, n) && !t->v[i].name[n]) return i;
  }
}

static void hashInsert(Table *t, int i) {
  const char *s = t->v[i].name;
  uint32_t h = hashName(s, (int)strlen(s)) & (t->hcap - 1);
  while (t->hash[h] >= 0) h = (h + 1) & (t->hcap - 1);
  t->hash[h] = i;
}

static void tableReset(Table *t) {
  t->n = 0;
  if (t->hash) memset(t->hash, 0xFF, t->hcap * sizeof *t->hash);
  t->str.cur = -1;
  t->str.used = 0;
}

// Adds a name, or gives a plain word already there the kind and declaration of a real one.
static void add(Table *t, const char *s, int n, uint8_t kind, const char *sig, int sigLen, int file) {
  if (n <= 0 || n > NAME_MAX) return;
  const int i = find(t, s, n);
  if (i >= 0) {
    Sym &y = t->v[i];
    if (y.kind == K_WORD && kind != K_WORD) {
      y.kind = kind;
      y.file = (int8_t)file;
      y.sig = sig ? arenaStr(&t->str, sig, sigLen) : nullptr;
    }
    return;
  }
  if (t->n == t->cap) {
    const int cap = t->cap ? t->cap * 2 : 256;
    Sym *v = (Sym *)psRealloc(t->v, cap * sizeof *v);
    if (!v) return;
    t->v = v;
    t->cap = cap;
  }
  if (2 * (t->n + 1) > t->hcap) {
    const int hcap = t->hcap ? t->hcap * 2 : 512;
    int *h = (int *)psRealloc(t->hash, hcap * sizeof *h);
    if (!h) return;
    t->hash = h;
    t->hcap = hcap;
    memset(h, 0xFF, hcap * sizeof *h);
    for (int j = 0; j < t->n; j++) hashInsert(t, j);
  }
  Sym &y = t->v[t->n];
  if (!(y.name = arenaStr(&t->str, s, n))) return;
  y.sig = sig ? arenaStr(&t->str, sig, sigLen) : nullptr;
  y.kind = kind;
  y.file = (int8_t)file;
  hashInsert(t, t->n++);
}

// ---- the scanner ----

struct Scanner {
  Table *t;
  int file;
  bool words;              // every name counts, not just declarations
  bool header;             // names starting with '_' are the C library's own: leave them out
  bool inComment, contLine;
  bool skip;               // inside #ifdef __cplusplus (extern "C" { would look like a body)
  int ifDepth;
  int brace, paren, bracket;
  char prev;               // last token: 'a' name, 'k' keyword, '"' string, '0' number, else itself
  // the file-scope statement so far
  char stmt[SIG_MAX + 1];
  int stmtLen;
  char last[NAME_MAX + 1], func[NAME_MAX + 1], tdef[NAME_MAX + 1];   // name candidates
  int lastLen, funcLen, tdefLen;
  bool lastHidden, funcHidden, tdefHidden;
  bool funcDone;           // the function's parameter list is closed
  bool body;               // the braces open are its body
  bool isTypedef, init, tag, sawEnum;
  int enumBrace;           // brace depth inside an enum's braces, 0 if not in one
};

static void scanInit(Scanner *sc, Table *t, int file, bool words, bool header) {
  memset(sc, 0, sizeof *sc);
  sc->t = t;
  sc->file = file;
  sc->words = words;
  sc->header = header;
}

static void endStmt(Scanner *sc) {
  sc->stmtLen = sc->lastLen = sc->funcLen = sc->tdefLen = 0;
  sc->funcDone = sc->body = sc->isTypedef = sc->init = sc->tag = sc->sawEnum = false;
}

static void append(Scanner *sc, const char *s, int n, bool space) {
  if (space && sc->stmtLen && sc->stmtLen < SIG_MAX) sc->stmt[sc->stmtLen++] = ' ';
  for (int i = 0; i < n && sc->stmtLen < SIG_MAX; i++) sc->stmt[sc->stmtLen++] = s[i];
}

static void keep(char *dst, int *dstLen, bool *hidden, const char *s, int n, bool h) {
  if (n > NAME_MAX) n = NAME_MAX;
  memcpy(dst, s, n);
  *dstLen = n;
  *hidden = h;
}

static void decl(Scanner *sc, const char *s, int n, uint8_t kind, const char *sig, int sigLen) {
  add(sc->t, s, n, kind, sig, sigLen, sc->file);
}

// A global: the last name, declared by the statement so far (without the = ; , [ that ends it).
static void varDecl(Scanner *sc) {
  if (!sc->lastLen || sc->lastHidden) { sc->lastLen = 0; return; }
  int n = sc->stmtLen;
  while (n && strchr("=;,[ ", sc->stmt[n - 1])) n--;
  decl(sc, sc->last, sc->lastLen, K_VAR, sc->stmt, n);
  sc->lastLen = 0;
}

// typedef ... name;  or  typedef ret (*name)(...);
static void typedefDecl(Scanner *sc) {
  if (sc->tdefLen) {
    if (!sc->tdefHidden) decl(sc, sc->tdef, sc->tdefLen, K_TYPE, nullptr, 0);
  } else if (sc->lastLen && !sc->lastHidden) {
    decl(sc, sc->last, sc->lastLen, K_TYPE, nullptr, 0);
  }
  sc->lastLen = sc->tdefLen = 0;
}

static void name(Scanner *sc, const char *s, int n, bool code, bool space, bool excluded) {
  const int kw = keyword(s, n);
  if (!code) {   // a directive's text
    if (kw < 0 && !excluded && sc->words && n > 1) decl(sc, s, n, K_WORD, nullptr, 0);
    return;
  }
  if (sc->brace == 0) append(sc, s, n, space);
  const bool top = sc->brace == 0 && sc->paren == 0 && sc->bracket == 0;
  const char before = sc->prev;
  sc->prev = 'a';
  if (kw >= 0) {
    sc->prev = 'k';
    if (!top) return;
    if (is(s, n, "typedef")) sc->isTypedef = true;
    else if (is(s, n, "struct") || is(s, n, "union") || is(s, n, "enum")) {
      sc->tag = true;
      if (s[0] == 'e') sc->sawEnum = true;
    }
    return;
  }
  const bool hidden = excluded || (sc->header && s[0] == '_');
  if (sc->tag) {
    sc->tag = false;
    if (!hidden) decl(sc, s, n, K_TYPE, nullptr, 0);
    return;
  }
  if (n > 1 && s[0] == '_' && s[1] == '_') { sc->prev = 'k'; return; }   // __attribute__ and such
  if (sc->enumBrace && sc->brace == sc->enumBrace && sc->paren == 0 && (before == '{' || before == ',')) {
    if (!hidden) decl(sc, s, n, K_MACRO, nullptr, 0);   // an enum constant
    return;
  }
  if (sc->words && !hidden && n > 1) decl(sc, s, n, K_WORD, nullptr, 0);
  if (top && !sc->init) keep(sc->last, &sc->lastLen, &sc->lastHidden, s, n, hidden);
  if (sc->isTypedef && sc->paren > 0 && before == '*' && !sc->tdefLen)
    keep(sc->tdef, &sc->tdefLen, &sc->tdefHidden, s, n, hidden);
}

static void punct(Scanner *sc, char c, bool space, char next, char before) {
  const bool top = sc->brace == 0 && sc->paren == 0 && sc->bracket == 0;
  if (sc->brace == 0) append(sc, &c, 1, space);
  switch (c) {
    case '(':
      if (top && sc->prev == 'a' && sc->lastLen && !sc->isTypedef && !sc->init && !sc->funcLen)
        keep(sc->func, &sc->funcLen, &sc->funcHidden, sc->last, sc->lastLen, sc->lastHidden);
      sc->paren++;
      break;
    case ')':
      if (sc->paren > 0) sc->paren--;
      if (sc->paren == 0 && sc->brace == 0 && sc->funcLen && !sc->funcDone) {
        sc->funcDone = true;
        if (!sc->funcHidden) decl(sc, sc->func, sc->funcLen, K_FUNC, sc->stmt, sc->stmtLen);
      }
      break;
    case '[':   // an array: its name is complete
      if (top && !sc->init) {
        if (!sc->isTypedef) {
          if (!sc->funcDone) varDecl(sc);
        } else if (!sc->tdefLen) {
          keep(sc->tdef, &sc->tdefLen, &sc->tdefHidden, sc->last, sc->lastLen, sc->lastHidden);
        }
      }
      sc->bracket++;
      break;
    case ']': if (sc->bracket > 0) sc->bracket--; break;
    case '{':
      if (sc->brace == 0) { sc->body = sc->funcDone; sc->tag = false; }
      if (sc->sawEnum && !sc->funcDone) sc->enumBrace = sc->brace + 1;
      sc->sawEnum = false;
      sc->brace++;
      break;
    case '}':
      if (sc->brace > 0) sc->brace--;
      if (sc->enumBrace > sc->brace) sc->enumBrace = 0;
      if (sc->brace == 0 && sc->body) endStmt(sc);
      break;
    case '=':
      if (top && next != '=' && !strchr("=!<>+-*/%&|^", before)) {
        if (!sc->isTypedef && !sc->funcDone) varDecl(sc);
        sc->init = true;
      }
      break;
    case ',':
      if (top) {
        if (sc->init) sc->init = false;
        else if (sc->isTypedef) typedefDecl(sc);
        else if (!sc->funcDone) varDecl(sc);
        sc->lastLen = 0;
      }
      break;
    case ';':
      if (top) {
        if (sc->isTypedef) typedefDecl(sc);
        else if (!sc->funcDone && !sc->init) varDecl(sc);
        endStmt(sc);
      }
      break;
  }
  sc->prev = c;
}

// Copies s[0, n) with runs of blanks made one space, comments cut off, into buf.
static int tidy(char *buf, const char *s, int n) {
  int m = 0;
  for (int i = 0; i < n && m < SIG_MAX; i++) {
    if (s[i] == '/' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '*')) break;
    if (s[i] == ' ' || s[i] == '\t' || s[i] == '\\') {
      if (m && buf[m - 1] != ' ') buf[m++] = ' ';
    } else buf[m++] = s[i];
  }
  while (m && buf[m - 1] == ' ') m--;
  return m;
}

static int readName(const char *s, int len, int *i) {
  while (*i < len && (s[*i] == ' ' || s[*i] == '\t')) (*i)++;
  const int a = *i;
  while (*i < len && identChar(s[*i])) (*i)++;
  return a;
}

// A directive line from its '#' at s[*i]. Returns whether the rest of the line (from *i) is
// scanned for words.
static bool directive(Scanner *sc, const char *s, int len, int *i) {
  int p = *i + 1;
  const int a = readName(s, len, &p);
  const char *d = s + a;
  const int dn = p - a;
  if ((is(d, dn, "ifdef") || is(d, dn, "if")) && contains(s, len, "__cplusplus")) {
    sc->skip = true;
    sc->ifDepth = 1;
    return false;
  }
  if (is(d, dn, "include")) return false;
  if (is(d, dn, "define")) {
    const int na = readName(s, len, &p);
    const int nn = p - na;
    if (nn && !(sc->header && s[na] == '_')) {
      int e = len;   // a function-like macro: just its parameters
      if (p < len && s[p] == '(') {
        const char *close = (const char *)memchr(s + p, ')', len - p);
        if (close) e = (int)(close - s) + 1;
      }
      char sig[SIG_MAX];
      decl(sc, s + na, nn, K_MACRO, sig, tidy(sig, s + na, e - na));
    }
  }
  *i = p;
  return sc->words;
}

static void skipDirective(Scanner *sc, const char *s, int len, int i) {
  int p = i + 1;
  const int a = readName(s, len, &p);
  if (p - a >= 2 && s[a] == 'i' && s[a + 1] == 'f') sc->ifDepth++;
  else if (is(s + a, p - a, "endif") && --sc->ifDepth == 0) sc->skip = false;
}

// One line. exX: the column of a name to leave out (the word being typed), or -1.
static void scanLine(Scanner *sc, const char *s, int len, int exX) {
  int i = 0;
  while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
  if (sc->skip) {
    if (i < len && s[i] == '#') skipDirective(sc, s, len, i);
    return;
  }
  bool code = true;
  if (!sc->inComment && (sc->contLine || (i < len && s[i] == '#'))) {
    const bool cont = sc->contLine;
    sc->contLine = len && s[len - 1] == '\\';
    if (!cont && !directive(sc, s, len, &i)) return;
    if (cont && !sc->words) return;
    code = false;
  } else {
    sc->contLine = false;
  }
  bool space = true;
  while (i < len) {
    if (sc->inComment) {
      while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
      if (i + 1 < len) { i += 2; sc->inComment = false; }
      else i = len;
      space = true;
      continue;
    }
    const char c = s[i];
    if (c == ' ' || c == '\t' || c == '\r') { i++; space = true; continue; }
    if (c == '/' && i + 1 < len && s[i + 1] == '/') break;
    if (c == '/' && i + 1 < len && s[i + 1] == '*') { sc->inComment = true; i += 2; space = true; continue; }
    const int start = i;
    if (c == '"' || c == '\'') {
      for (i++; i < len && s[i] != c; i++)
        if (s[i] == '\\') i++;
      if (i < len) i++;
      if (i > len) i = len;
      if (code) {
        if (sc->brace == 0) append(sc, s + start, i - start, space);
        sc->prev = '"';
      }
    } else if (isDigit(c)) {
      while (i < len && (identChar(s[i]) || s[i] == '.')) i++;
      if (code) {
        if (sc->brace == 0) append(sc, s + start, i - start, space);
        sc->prev = '0';
      }
    } else if (identChar(c)) {
      while (i < len && identChar(s[i])) i++;
      name(sc, s + start, i - start, code, space, start == exX);
    } else {
      i++;
      if (code) punct(sc, c, space, i < len ? s[i] : 0, start > 0 ? s[start - 1] : 0);
    }
    space = false;
  }
}

static void scanFile(Table *t, const char *path, int file, bool header) {
  FILE *f = fopen(path, "rb");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = size > 0 && size < 512 * 1024 ? (char *)psRealloc(nullptr, size) : nullptr;
  const int n = buf ? (int)fread(buf, 1, size, f) : 0;
  fclose(f);
  Scanner sc;
  scanInit(&sc, t, file, false, header);
  for (int p = 0; p < n;) {
    const char *eol = (const char *)memchr(buf + p, '\n', n - p);
    int e = eol ? (int)(eol - buf) : n, l = e - p;
    if (l && buf[p + l - 1] == '\r') l--;
    scanLine(&sc, buf + p, l, -1);
    p = e + 1;
  }
  free(buf);
}

// ---- the indexes ----

static Table s_hdr, s_proj, s_live;
static char s_incDir[128], s_projDir[192];
static bool s_hdrOk, s_projOk;
static char *s_files[MAX_FILES];
static int s_nfiles;
static bool s_fileOpen[MAX_FILES];

static bool isDir(const char *path) {
  struct stat st;
  return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static bool endsWith(const char *s, const char *end) {
  const size_t a = strlen(s), b = strlen(end);
  return a > b && !strcmp(s + a - b, end);
}

// The .h files (or .c/.h with `sources`) of a folder, malloc'ed full paths; folders too if depth > 0.
static int listDir(const char *dir, bool sources, int depth, char **out, int max) {
  char *names[64];
  int nn = 0, n = 0;
  if (DIR *dp = opendir(dir)) {
    while (struct dirent *e = readdir(dp))
      if (e->d_name[0] != '.' && nn < 64) names[nn++] = strdup(e->d_name);
    closedir(dp);
  }
  for (int i = 0; i < nn; i++) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, names[i]);
    free(names[i]);
    if (endsWith(path, ".h") || (sources && endsWith(path, ".c"))) {
      if (n < max) out[n++] = strdup(path);
    } else if (depth > 0 && isDir(path)) {
      n += listDir(path, sources, depth - 1, out + n, max - n);
    }
  }
  return n;
}

static void buildIndexes() {
  if (!s_hdrOk) {
    tableReset(&s_hdr);
    for (int i = 0; i < N_KEYWORDS; i++)
      add(&s_hdr, kKeywords[i].s, (int)strlen(kKeywords[i].s), kKeywords[i].kind, nullptr, 0, -1);
    char *files[MAX_FILES];
    const int n = s_incDir[0] ? listDir(s_incDir, false, 1, files, MAX_FILES) : 0;
    for (int i = 0; i < n; i++) {
      scanFile(&s_hdr, files[i], -1, true);
      free(files[i]);
    }
    s_hdrOk = true;
  }
  if (!s_projOk) {
    tableReset(&s_proj);
    for (int i = 0; i < s_nfiles; i++) free(s_files[i]);
    s_nfiles = s_projDir[0] ? listDir(s_projDir, true, 0, s_files, MAX_FILES) : 0;
    for (int i = 0; i < s_nfiles; i++) scanFile(&s_proj, s_files[i], i, false);
    s_projOk = true;
  }
}

// Reads the open files again, leaving out the name at (cur, its cursor line, exX), and notes
// which project files are open (their saved text doesn't count).
static void readOpenFiles(Doc *const *docs, int ndocs, const Doc *cur, int exX) {
  buildIndexes();
  tableReset(&s_live);
  for (int i = 0; i < ndocs; i++) {
    const Doc *d = docs[i];
    Scanner sc;
    scanInit(&sc, &s_live, -1, true, false);
    for (int y = 0; y < d->n; y++) scanLine(&sc, d->ln[y].s, d->ln[y].len, d == cur && y == cur->cy ? exX : -1);
  }
  for (int f = 0; f < s_nfiles; f++) {
    s_fileOpen[f] = false;
    for (int i = 0; i < ndocs && !s_fileOpen[f]; i++) s_fileOpen[f] = !strcmp(docs[i]->path, s_files[f]);
  }
}

// The declaration of a name that counts: the open files', else the project's other files', else
// the headers'. Null if it is only a word.
static const Sym *declOf(const char *s, int n) {
  int i = find(&s_live, s, n);
  if (i >= 0 && s_live.v[i].kind != K_WORD) return &s_live.v[i];
  i = find(&s_proj, s, n);
  if (i >= 0 && !s_fileOpen[s_proj.v[i].file]) return &s_proj.v[i];
  i = find(&s_hdr, s, n);
  return i >= 0 ? &s_hdr.v[i] : nullptr;
}

void ac_project(const char *includeDir, const char *projDir) {
  ac_close();
  if (strcmp(includeDir, s_incDir)) {
    snprintf(s_incDir, sizeof s_incDir, "%s", includeDir);
    s_hdrOk = false;
  }
  snprintf(s_projDir, sizeof s_projDir, "%s", projDir);
  s_projOk = false;
}

// ---- the list ----

struct Item {
  const Sym *s;
  uint8_t group;           // 0: the case matches too
};

static Item *s_items;
static int s_nitems, s_itemsCap, s_sel, s_top;
static bool s_open;
static int s_wy, s_wx;     // where the word starts
static char s_word[NAME_MAX + 1];
static int s_wlen;

bool ac_active() { return s_open; }

void ac_close() {
  s_open = false;
}

static int cmpItem(const void *a, const void *b) {
  const Item *x = (const Item *)a, *y = (const Item *)b;
  if (x->group != y->group) return x->group - y->group;
  return strcmp(x->s->name, y->s->name);
}

static void addItem(const Sym *y, uint8_t group) {
  if (s_nitems == s_itemsCap) {
    const int cap = s_itemsCap ? s_itemsCap * 2 : 128;
    Item *v = (Item *)psRealloc(s_items, cap * sizeof *v);
    if (!v) return;
    s_items = v;
    s_itemsCap = cap;
  }
  s_items[s_nitems++] = {y, group};
}

static void collect(const Table *t) {
  for (int i = 0; i < t->n; i++) {
    const Sym *y = &t->v[i];
    if (strncasecmp(y->name, s_word, s_wlen)) continue;
    const Sym *d = declOf(y->name, (int)strlen(y->name));
    if (d ? d != y : y->kind != K_WORD) continue;   // listed from where it counts
    addItem(y, strncmp(y->name, s_word, s_wlen) ? 1 : 0);
  }
}

static int wordStart(const Doc *d) {
  const DocLine *l = &d->ln[d->cy];
  int x = d->cx;
  while (x > 0 && identChar(l->s[x - 1])) x--;
  return x;
}

// Lists the names that fit the word before the cursor. The open files are read again only when
// `docs` is given (Ctrl-Space); narrowing the list while typing reuses what was read. False (and
// the list closed) if no name fits, or only the word itself.
static bool refresh(Doc *const *docs, int ndocs, Doc *d) {
  const int x0 = wordStart(d), n = d->cx - x0;
  if (n > NAME_MAX || (n && isDigit(d->ln[d->cy].s[x0]))) { ac_close(); return false; }
  memcpy(s_word, d->ln[d->cy].s + x0, n);
  s_word[n] = 0;
  s_wlen = n;
  s_wy = d->cy;
  s_wx = x0;
  if (docs) readOpenFiles(docs, ndocs, d, x0);
  s_nitems = 0;
  collect(&s_live);
  collect(&s_proj);
  collect(&s_hdr);
  qsort(s_items, s_nitems, sizeof *s_items, cmpItem);
  if (!s_nitems || (s_nitems == 1 && !strcmp(s_items[0].s->name, s_word))) { ac_close(); return false; }
  s_open = true;
  s_sel = s_top = 0;
  return true;
}

static void accept(Doc *d, int rows) {
  doc_begin(d);
  d->marking = false;
  d->sel = true;
  d->ay = s_wy;
  d->ax = s_wx;
  ed_replace_sel(d, s_items[s_sel].s->name, rows);
  ac_close();
}

bool ac_open(Doc *const *docs, int ndocs, Doc *d, int rows) {
  ed_unselect(d);
  if (!refresh(docs, ndocs, d)) return false;
  if (s_nitems == 1) accept(d, rows);
  return true;
}

bool ac_key(Doc *d, int key, int rows) {
  if (!s_open) return false;
  const int k = key & 0xFF, n = s_nitems;
  if (!(key & (UI_CTRL | UI_ALT | UI_SHIFT))) {
    switch (k) {
      case KEY_UP: s_sel = (s_sel + n - 1) % n; return true;
      case KEY_DOWN: s_sel = (s_sel + 1) % n; return true;
      case KEY_PGUP: s_sel = s_sel >= LIST_ROWS ? s_sel - LIST_ROWS : 0; return true;
      case KEY_PGDN: s_sel = s_sel + LIST_ROWS < n ? s_sel + LIST_ROWS : n - 1; return true;
      case KEY_ESC: ac_close(); return true;
      case KEY_TAB: accept(d, rows); return true;
      case ' ': accept(d, rows); return false;   // the editor then types the space after it
      case KEY_ENTER:
        // the word is already the chosen name: Enter is a new line
        if (strcmp(s_items[s_sel].s->name, s_word)) { accept(d, rows); return true; }
        break;
      default:
        if (identChar(k) || k == KEY_BACKSPACE) return false;   // ac_after_key() narrows the list
    }
  }
  if (key == (' ' | UI_CTRL)) return true;
  ac_close();
  return false;
}

void ac_after_key(Doc *d, int key) {
  if (!s_open) return;
  const int k = key & 0xFF;
  const bool plain = !(key & (UI_CTRL | UI_ALT));
  const bool typed = plain && identChar(k), back = plain && k == KEY_BACKSPACE;
  // Backspace over the word's first letter closes the list
  if ((typed || (back && d->cx > s_wx)) && d->cy == s_wy && d->cx >= s_wx && !ed_has_sel(d))
    refresh(nullptr, 0, d);
  else ac_close();
}

void ac_draw(const Doc *d, int y0, int rows) {
  if (!s_open) return;
  const int n = s_nitems, vis = n < LIST_ROWS ? n : LIST_ROWS;
  if (s_sel < s_top) s_top = s_sel;
  if (s_sel >= s_top + vis) s_top = s_sel - vis + 1;
  int cols = 8;
  for (int i = 0; i < n; i++) {
    const int l = (int)strlen(s_items[i].s->name);
    if (l > cols) cols = l < LIST_COLS ? l : LIST_COLS;
  }
  const int w = cols * UI_CW + 5 + (n > vis ? UI_SCROLL_W : 0), h = vis * UI_CH + 2;
  int x = ED_X0 + (s_wx - d->left) * UI_CW - 3;
  if (x + w > SCREEN_W) x = SCREEN_W - w;
  if (x < 0) x = 0;
  const int r = d->cy - d->top;   // below the cursor line, or above it if there's no room
  int y = y0 + (r + 1) * UI_CH;
  if (r + 1 + vis > rows && r >= vis) y = y0 + r * UI_CH - h;
  gfx_font(FONT_6X10);
  gfx_fill_rect(x, y, w, h, RGB(30, 30, 46));
  gfx_rect(x, y, w, h, LIGHTGRAY);
  char buf[LIST_COLS + 1];
  for (int i = 0; i < vis; i++) {
    const Item &it = s_items[s_top + i];
    const int yy = y + 1 + i * UI_CH;
    if (s_top + i == s_sel) gfx_fill_rect(x + 1, yy, w - 2, UI_CH, UI_SEL);
    snprintf(buf, sizeof buf, "%s", it.s->name);
    gfx_text(x + 3, yy, buf, kKindColors[it.s->kind]);
  }
  if (n > vis) ui_scrollbar(x + w - 1 - UI_SCROLL_W, y + 1, h - 2, n, vis, s_top);
}

const char *ac_hint() { return s_open ? s_items[s_sel].s->sig : nullptr; }
