// alloc.c - compiler arena, diagnostics, small host helpers and the cc_compile() driver.
#include "chibicc.h"
#ifdef ESP_PLATFORM
# include "esp_heap_caps.h"
#endif

//
// Arena: bump allocation in 4 KB chunks; big requests get a chunk of their own.
//
// There are four. The object arena holds finished CcObjects until cc_arena_free(). The main
// arena holds one compile (tokens, AST, codegen buffers) and is dropped once its object has been
// copied out. The scratch arena holds the preprocessor's working set (raw tokens, macro bodies,
// expansion copies) and is dropped as soon as the surviving tokens have been copied out
// (cc_scratch_begin/end, keep_token()). The function arena holds one function's Nodes and locals
// while it is parsed and compiled (cc_fn_begin/end); its Nodes are kept apart, in the node arena,
// so the ones of a statement packed once parsed can be dropped (cc_node_release()).
//

#define CHUNK_SIZE (4 * 1024)

typedef struct Chunk Chunk;
struct Chunk {
  Chunk *next;
  size_t size, used;
  _Alignas(8) unsigned char data[];
};

typedef struct {
  Chunk *chunks;
  size_t used;
  void *last_alloc;  // most recent allocation, which cc_realloc can grow in place
} Arena;

static Arena obj_arena, main_arena, scratch_arena, fn_arena, node_arena;
static Arena *arena = &main_arena;  // where allocations go now
static bool scratch_open, fn_open, fn_types;
static size_t arena_reserved, arena_peak;  // chunk bytes held now / at most since cc_arena_free

static void *raw_alloc(size_t n) {
#ifdef ESP_PLATFORM
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
#else
  return malloc(n);
#endif
}

static void *raw_realloc(void *p, size_t n) {
#ifdef ESP_PLATFORM
  void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return q ? q : realloc(p, n);
#else
  return realloc(p, n);
#endif
}

// Lent memory (cc_arena_lend): a first-fit heap of its own, tried first for arena chunks and
// grown arrays alike, so the big arrays don't have to find room in the system heap. Every block
// starts with its size (header included); free ones are listed in address order and merged.
typedef struct LentFree LentFree;
struct LentFree {
  size_t size;
  LentFree *next;
};

#define LENT_HDR 8      // keeps the payload 8-aligned
#define LENT_MIN 64     // a smaller remainder stays with the block

static unsigned char *lent_lo, *lent_hi;
static LentFree *lent_free;
static size_t lent_used, lent_peak;  // block bytes, headers included

size_t cc_lent_peak(void) { return lent_peak; }

void cc_arena_lend(void *buf, size_t n) {
  lent_free = NULL;
  lent_used = lent_peak = 0;
  lent_lo = lent_hi = NULL;
  unsigned char *lo = (unsigned char *)(((uintptr_t)buf + 7) & ~(uintptr_t)7);
  if (!buf || (unsigned char *)buf + n < lo + LENT_MIN)
    return;
  n = ((unsigned char *)buf + n - lo) & ~(size_t)7;
  lent_lo = lo;
  lent_hi = lo + n;
  lent_free = (LentFree *)lo;
  lent_free->size = n;
  lent_free->next = NULL;
}

static bool is_lent(void *p) {
  return (unsigned char *)p >= lent_lo && (unsigned char *)p < lent_hi;
}

bool cc_arena_is_lent(const void *p) { return is_lent((void *)p); }

static void *lent_alloc(size_t n) {
  n = (n + LENT_HDR + 7) & ~(size_t)7;
  for (LentFree **pp = &lent_free; *pp; pp = &(*pp)->next) {
    LentFree *b = *pp;
    if (b->size < n)
      continue;
    if (b->size - n >= LENT_MIN) {
      LentFree *rest = (LentFree *)((unsigned char *)b + n);
      rest->size = b->size - n;
      rest->next = b->next;
      *pp = rest;
    } else {
      n = b->size;
      *pp = b->next;
    }
    *(size_t *)b = n;
    if ((lent_used += n) > lent_peak)
      lent_peak = lent_used;
    return (unsigned char *)b + LENT_HDR;
  }
  return NULL;
}

static void lent_release(void *p) {
  LentFree *b = (LentFree *)((unsigned char *)p - LENT_HDR);
  b->size = *(size_t *)b;
  lent_used -= b->size;
  LentFree **pp = &lent_free, *prev = NULL;
  while (*pp && *pp < b) {
    prev = *pp;
    pp = &(*pp)->next;
  }
  b->next = *pp;
  *pp = b;
  if (b->next && (unsigned char *)b + b->size == (unsigned char *)b->next) {
    b->size += b->next->size;
    b->next = b->next->next;
  }
  if (prev && (unsigned char *)prev + prev->size == (unsigned char *)b) {
    prev->size += b->size;
    prev->next = b->next;
  }
}

// Gives the end of a lent block beyond n payload bytes back.
static void lent_trim(void *p, size_t n) {
  size_t *hdr = (size_t *)((unsigned char *)p - LENT_HDR);
  n = (n + LENT_HDR + 7) & ~(size_t)7;
  if (*hdr - n < LENT_MIN)
    return;
  size_t *tail = (size_t *)((unsigned char *)hdr + n);
  *tail = *hdr - n;
  *hdr = n;
  lent_release((unsigned char *)tail + LENT_HDR);
}

static void *mem_alloc(size_t n) {
  void *p = lent_alloc(n);
  return p ? p : raw_alloc(n);
}

// Outlives the arenas: by then the heap is full of the compiler's own memory, and the lent memory
// mostly free.
void *cc_image_alloc(size_t n) {
  void *p = lent_alloc(n);
  return p ? p : malloc(n);
}

static void mem_free(void *p) {
  if (is_lent(p))
    lent_release(p);
  else
    free(p);
}

// Moves p (old bytes, or NULL) to a block of n bytes; NULL if there is no room (p is kept).
static void *mem_resize(void *p, size_t old, size_t n) {
  void *q = lent_alloc(n);
  if (!q) {
    // realloc() can fail where malloc() of the new size works (seen with the RP2350's newlib):
    // then copy by hand.
    if (p && !is_lent(p) && (q = raw_realloc(p, n)))
      return q;
    if (!(q = raw_alloc(n)))
      return NULL;
  }
  if (p) {
    memcpy(q, p, old < n ? old : n);
    mem_free(p);
  }
  return q;
}

static Chunk *chunk_alloc(size_t size) { return mem_alloc(sizeof(Chunk) + size); }

void (*cc_oom_hook)(const char *what, size_t n, size_t lent_free, size_t lent_largest);

noreturn void cc_out_of_memory(const char *what, size_t n) {
  if (cc_oom_hook) {
    size_t total = 0, largest = 0;
    for (LentFree *b = lent_free; b; b = b->next) {
      total += b->size;
      if (b->size > largest)
        largest = b->size;
    }
    cc_oom_hook(what, n, total, largest);
  }
  error("out of memory");
}

static void chunk_free(Chunk *c) {
  arena_reserved -= sizeof(Chunk) + c->size;
  mem_free(c);
}

static void note_reserved(size_t n) {
  arena_reserved += n;
  if (arena_reserved > arena_peak)
    arena_peak = arena_reserved;
}

static void *arena_alloc(size_t n) {
  Arena *a = arena;
  n = (n + 7) & ~(size_t)7;
  if (!a->chunks || a->chunks->size - a->chunks->used < n) {
    size_t size = n > CHUNK_SIZE / 4 ? n : CHUNK_SIZE;
    Chunk *c = chunk_alloc(size);
    if (!c)
      cc_out_of_memory("chunk", sizeof(Chunk) + size);
    note_reserved(sizeof(Chunk) + size);
    c->size = size;
    c->used = 0;
    if (a->chunks && size != CHUNK_SIZE) {
      // Keep allocating from the current chunk; slot the big one in behind it.
      c->used = size;
      c->next = a->chunks->next;
      a->chunks->next = c;
      a->used += n;
      a->last_alloc = NULL;
      return c->data;
    }
    c->next = a->chunks;
    a->chunks = c;
  }
  void *p = a->chunks->data + a->chunks->used;
  a->chunks->used += n;
  a->used += n;
  a->last_alloc = p;
  return p;
}

void *cc_calloc(size_t n, size_t size) {
  size_t total = n * size;
  void *p = arena_alloc(total ? total : 1);
  memset(p, 0, total);
  return p;
}

void *cc_realloc(void *p, size_t old_size, size_t new_size) {
  Arena *a = arena;
  if (p && p == a->last_alloc) {
    size_t old_r = (old_size + 7) & ~(size_t)7, new_r = (new_size + 7) & ~(size_t)7;
    if (a->chunks->used - old_r + new_r <= a->chunks->size) {
      a->chunks->used = a->chunks->used - old_r + new_r;
      a->used = a->used - old_r + new_r;
      if (new_size > old_size)
        memset((char *)p + old_size, 0, new_size - old_size);
      return p;
    }
  }
  void *q = cc_calloc(1, new_size);
  if (p)
    memcpy(q, p, old_size < new_size ? old_size : new_size);
  return q;
}

char *cc_strndup(const char *s, size_t n) {
  size_t len = strnlen(s, n);
  char *p = arena_alloc(len + 1);
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

char *cc_strdup(const char *s) { return cc_strndup(s, strlen(s)); }

static void free_arena(Arena *a) {
  while (a->chunks) {
    Chunk *next = a->chunks->next;
    chunk_free(a->chunks);
    a->chunks = next;
  }
  a->used = 0;
  a->last_alloc = NULL;
}

//
// Growable arrays (code, relocations, symbols, the line table, the assembler's labels): on the
// heap rather than in an arena, so growing one frees the old copy instead of leaving it behind.
// Like the main arena they belong to one compile.
//

static Token **tracked;  // see cc_tok_alloc()
static int ntracked, tracked_cap;
static bool tracking;

typedef struct Grown Grown;
struct Grown {
  Grown *next, *prev;
  size_t size;
  _Alignas(8) unsigned char data[];
};

static Grown grown = {&grown, &grown, 0};

// Like cc_realloc(), for an array that only cc_grow() allocates.
void *cc_grow(void *p, size_t old_size, size_t new_size) {
  Grown *g = p ? (Grown *)((unsigned char *)p - offsetof(Grown, data)) : NULL;
  Grown *h = mem_resize(g, g ? sizeof(Grown) + g->size : 0, sizeof(Grown) + new_size);
  if (!h)
    cc_out_of_memory("array", sizeof(Grown) + new_size);
  if (g) {
    arena_reserved -= sizeof(Grown) + h->size;
  } else {
    h->next = grown.next;
    h->prev = &grown;
    old_size = 0;
  }
  h->next->prev = h->prev->next = h;
  note_reserved(sizeof(Grown) + new_size);
  h->size = new_size;
  if (new_size > old_size)
    memset(h->data + old_size, 0, new_size - old_size);
  return h->data;
}

void cc_grow_free(void *p) {
  if (!p)
    return;
  Grown *g = (Grown *)((unsigned char *)p - offsetof(Grown, data));
  g->prev->next = g->next;
  g->next->prev = g->prev;
  arena_reserved -= sizeof(Grown) + g->size;
  mem_free(g);
}

static void free_grown(void) {
  tracked = NULL;
  ntracked = tracked_cap = 0;
  tracking = false;
  while (grown.next != &grown) {
    Grown *g = grown.next;
    grown.next = g->next;
    arena_reserved -= sizeof(Grown) + g->size;
    mem_free(g);
  }
  grown.prev = &grown;
}

// Token slots the compiler is done with: raw tokens the preprocessor has consumed, and the tokens
// of each top-level declaration once it is parsed. Any new token reuses them, so memory follows
// the largest declaration, not the program. The scratch arena outlives every token (it is dropped
// last), so a slot from either arena can be reused for anything.
static void *tok_free;

// While tracking (a top-level function-like macro expansion), new tokens are also recorded, and
// cc_tok_sweep() frees all but the marked ones (the expansion): the argument copies and the
// expansions of the arguments are garbage once it is done. Freeing is left to the sweep meanwhile.

void *cc_tok_alloc(void) {
  void *p;
  if (tok_free) {
    p = tok_free;
    tok_free = *(void **)p;
    memset(p, 0, sizeof(Token));
  } else {
    p = cc_calloc(1, sizeof(Token));
  }
  if (tracking) {
    if (ntracked == tracked_cap) {
      int cap = tracked_cap ? tracked_cap * 2 : 64;
      tracked = cc_grow(tracked, tracked_cap * sizeof(Token *), cap * sizeof(Token *));
      tracked_cap = cap;
    }
    tracked[ntracked++] = p;
  }
  return p;
}

void cc_tok_free(void *p) {
  if (tracking)
    return;
  *(void **)p = tok_free;
  tok_free = p;
}

bool cc_tok_track(bool on) {
  bool was = tracking;
  tracking = on;
  return was;
}

void cc_tok_sweep(void) {
  tracking = false;
  for (int i = 0; i < ntracked; i++) {
    if (tracked[i]->marked)
      tracked[i]->marked = false;
    else
      cc_tok_free(tracked[i]);
  }
  ntracked = 0;
}

void cc_scratch_begin(void) {
  scratch_open = true;
  arena = &scratch_arena;
}

void cc_scratch_end(void) {
  free_arena(&scratch_arena);
  tok_free = NULL;
  scratch_open = false;
  arena = &main_arena;
}

void cc_fn_begin(void) { fn_open = fn_types = true; }

bool cc_fn_is_open(void) { return fn_open; }

static CcArenaMark arena_mark(Arena *a) {
  Chunk *c = a->chunks;
  return (CcArenaMark){c, c ? c->next : NULL, c ? c->used : 0, a->used};
}

static void arena_release(Arena *a, CcArenaMark m) {
  Chunk *top = m.chunk;
  // Chunks started since are in front of the marked one; big ones may sit right behind it.
  while (a->chunks != top) {
    Chunk *next = a->chunks->next;
    chunk_free(a->chunks);
    a->chunks = next;
  }
  if (top) {
    while (top->next != m.next) {
      Chunk *c = top->next;
      top->next = c->next;
      chunk_free(c);
    }
    top->used = m.used;
  }
  a->used = m.total;
  a->last_alloc = NULL;
}

// What the function and node arenas hold now; cc_fn_release() drops what is allocated after it.
CcFnMark cc_fn_mark(void) {
  return (CcFnMark){arena_mark(&fn_arena), arena_mark(&node_arena)};
}

void cc_fn_release(CcFnMark m) {
  arena_release(&fn_arena, m.fn);
  arena_release(&node_arena, m.node);
}

void cc_node_release(CcFnMark m) {
  arena_release(&node_arena, m.node);
}

void cc_fn_end(void) {
  free_arena(&node_arena);
  free_arena(&fn_arena);
  fn_open = fn_types = false;
}

// While a function is open, the Types made in its body go in the function arena too, except
// struct, union and enum types (see declspec()); new_gvar() copies out those a global needs.
bool cc_fn_types(bool on) {
  bool was = fn_types;
  fn_types = on && fn_open;
  return was;
}

void *cc_type_calloc(size_t size) {
  return fn_types ? cc_fn_calloc(size) : cc_calloc(1, size);
}

bool cc_in_fn_arena(void *p) {
  for (Chunk *c = fn_arena.chunks; c; c = c->next)
    if ((unsigned char *)p >= c->data && (unsigned char *)p < c->data + c->size)
      return true;
  return false;
}

// A zeroed Node or local variable: in the function arena while one is open.
void *cc_fn_calloc(size_t size) {
  if (!fn_open)
    return cc_calloc(1, size);
  Arena *a = arena;
  arena = &fn_arena;
  void *p = cc_calloc(1, size);
  arena = a;
  return p;
}

void *cc_node_calloc(size_t size) {
  if (!fn_open)
    return cc_calloc(1, size);
  Arena *a = arena;
  arena = &node_arena;
  void *p = cc_calloc(1, size);
  arena = a;
  return p;
}

bool cc_keep(bool on) {
  bool was = arena == &main_arena;
  arena = on || !scratch_open ? &main_arena : &scratch_arena;
  return was;
}

static void free_kept(void);

void cc_arena_free(void) {
  cc_scratch_end();
  cc_fn_end();
  free_arena(&main_arena);
  free_grown();
  free_kept();
  free_arena(&obj_arena);
  arena_reserved = arena_peak = 0;
}

size_t cc_arena_used(void) { return obj_arena.used + main_arena.used + scratch_arena.used; }
size_t cc_arena_peak(void) { return arena_peak; }

//
// Diagnostics
//

void (*cc_diag_hook)(const char *msg);

void cc_diag(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (cc_diag_hook)
    cc_diag_hook(buf);
  else
    fputs(buf, stderr);
}

const char *cc_error(void) { return cc_errbuf; }

size_t cc_stack_limit;
static char *stack_floor;  // cc_stack_limit below cc_compile()'s frame

// Called where the parser, preprocessor and code generator recurse.
void check_stack(Token *tok) {
  if ((char *)__builtin_frame_address(0) < stack_floor) {
    if (tok)
      error_tok(tok, "nested too deeply");
    error("nested too deeply");
  }
}

//
// Files
//

StringArray include_paths;
bool opt_fcommon = true;
char *base_file;

bool file_exists(char *path) {
  if (cc_read_file_hook) {
    size_t len;
    if (cc_read_file_hook(path, &len))
      return true;
  }
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return false;
  fclose(fp);
  return true;
}

char *cc_dirname(char *path) {
  char *slash = strrchr(path, '/');
#ifdef _WIN32
  char *bs = strrchr(path, '\\');
  if (bs > slash)
    slash = bs;
#endif
  if (!slash)
    return ".";
  if (slash == path)
    return "/";
  return strndup(path, slash - path);
}

// Drops "." and "dir/.." from a path, in place: "#include "../lib/x.h"" names another project's
// header, and the SD card's file system doesn't know "..".
char *cc_normpath(char *path) {
  int starts[64], n = 0;   // where the kept segments start
  char *w = path;
  const char *r = path;
  if (*r == '/') {
    *w++ = '/';
    while (*r == '/')
      r++;
  }
  char *base = w;
  while (*r) {
    const char *e = r;
    while (*e && *e != '/')
      e++;
    size_t len = e - r;
    if (len == 1 && r[0] == '.') {
      // nothing
    } else if (len == 2 && r[0] == '.' && r[1] == '.' && n > 0) {
      w = path + starts[--n];
    } else {
      if (w > base && w[-1] != '/')
        *w++ = '/';
      if (!(len == 2 && r[0] == '.' && r[1] == '.') && n < 64)   // a leading ".." stays
        starts[n++] = w - path;
      memmove(w, r, len);
      w += len;
    }
    r = e;
    while (*r == '/')
      r++;
  }
  if (w > base && w[-1] == '/')
    w--;
  *w = 0;
  return path;
}

//
// Driver
//

// Grown arrays a finished object took over (cc_grow_keep()); they go with the object arena.
static Grown kept = {&kept, &kept, 0};

// Hands a grown array of n bytes (trimmed to them) to the object, so it is not freed, nor
// copied, with the compile.
void *cc_grow_keep(void *p, size_t n) {
  if (!p)
    return NULL;
  Grown *g = (Grown *)((unsigned char *)p - offsetof(Grown, data));
  g->prev->next = g->next;
  g->next->prev = g->prev;
  arena_reserved -= sizeof(Grown) + g->size;
  Grown *h = g;
  if (n < g->size && is_lent(g)) {
    lent_trim(g, sizeof(Grown) + n);
    g->size = n;
  } else if (n < g->size && (h = raw_realloc(g, sizeof(Grown) + n))) {
    h->size = n;
  } else {
    h = g;  // as it was
  }
  h->next = kept.next;
  h->prev = &kept;
  h->next->prev = h->prev->next = h;
  note_reserved(sizeof(Grown) + h->size);
  return h->data;
}

static void free_kept(void) {
  while (kept.next != &kept) {
    Grown *g = kept.next;
    kept.next = g->next;
    arena_reserved -= sizeof(Grown) + g->size;
    mem_free(g);
  }
  kept.prev = &kept;
}

// Moves a finished object out of the compile, whose arenas can then be dropped: linking needs
// only the objects, and the next file's compile can reuse the memory. Its arrays are taken over
// rather than copied, so this needs no more memory than the compile had.
static CcObject *keep_object(CcObject *o) {
  arena = &obj_arena;
  CcObject *k = cc_calloc(1, sizeof(CcObject));
  *k = *o;
  k->path = cc_strdup(o->path);
  for (int i = 0; i < o->nsyms; i++) {
    CcSym *s = &o->syms[i];
    // The linker looks up only global, undefined and common symbols by name.
    s->name = s->sect >= 0 && !s->is_global && !s->is_common ? "" : cc_strdup(s->name);
  }
  for (int i = 0; i < o->nfiles; i++)
    o->files[i] = cc_strdup(o->files[i]);
  for (int i = 0; i < o->nfuncs; i++)
    o->funcs[i].name = cc_strdup(o->funcs[i].name);
  cc_scratch_end();
  free_arena(&main_arena);

  for (int i = 0; i < CC_NSECT; i++) {
    CcSection *s = &k->sect[i];
    s->data = cc_grow_keep(s->data, s->size - s->base);
    s->cap = s->size - s->base;
    s->parts = cc_grow_keep(s->parts, sizeof(CcPart) * s->nparts);  // their data is kept already
  }
  k->syms = cc_grow_keep(k->syms, sizeof(CcSym) * k->nsyms);
  k->capsyms = k->nsyms;
  int nb = (k->nrelocs + CC_RELOC_BLOCK - 1) / CC_RELOC_BLOCK;  // blocks past these go below
  for (int b = 0; b < nb; b++)
    k->relocs[b] = cc_grow_keep(k->relocs[b], sizeof(CcReloc) * (b < nb - 1 ? CC_RELOC_BLOCK : k->nrelocs - b * CC_RELOC_BLOCK));
  k->relocs = cc_grow_keep(k->relocs, sizeof(CcReloc *) * nb);
  k->caprelocs = k->nrelocs;
  k->lines = cc_grow_keep(k->lines, sizeof(CcLine) * k->nlines);
  k->caplines = k->nlines;
  k->files = cc_grow_keep(k->files, sizeof(char *) * k->nfiles);
  k->capfiles = k->nfiles;
  k->stops = cc_grow_keep(k->stops, sizeof(uint32_t) * k->nstops);
  k->capstops = k->nstops;
  k->funcs = cc_grow_keep(k->funcs, sizeof(CcFunc) * k->nfuncs);
  k->capfuncs = k->nfuncs;
  free_grown();
  return k;
}

int cc_compile(const CcOptions *opt, const char *path, CcObject **out) {
  *out = NULL;
  cc_errbuf[0] = '\0';
  stack_floor = cc_stack_limit ? (char *)__builtin_frame_address(0) - cc_stack_limit : NULL;
  if (setjmp(cc_error_jmp)) {
    cc_scratch_end();
    cc_fn_end();
    free_arena(&main_arena);
    free_grown();
    return -1;
  }

  tokenize_reset();
  preprocess_reset();
  parse_reset();

  include_paths = (StringArray){0};
  for (int i = 0; i < opt->n_include_paths; i++)
    strarray_push(&include_paths, strdup(opt->include_paths[i]));

  char *file = strdup(path);
  base_file = file;
  // Everything the preprocessor builds goes in the scratch arena; only its output tokens (and the
  // Files they point into) are copied out to main for the parser.
  cc_scratch_begin();
  init_macros();
  for (int i = 0; i < opt->n_defines; i++) {
    char *def = strdup(opt->defines[i]);
    char *eq = strchr(def, '=');
    if (eq) {
      *eq = '\0';
      define_macro(def, eq + 1);
    } else {
      define_macro(def, "1");
    }
  }

  Token *tok = tokenize_file(file, NULL);
  if (!tok)
    error("cannot open %s", file);
  // The preprocessor hands the parser one top-level declaration at a time (preprocess_more()),
  // and functions are compiled as soon as they are parsed (see parse.c function()).
  tok = preprocess(tok);
  cc_keep(true);
  CcObject *obj = calloc(1, sizeof(CcObject));
  obj->path = file;
  codegen_begin(obj);
  codegen_end(parse(tok));
  *out = keep_object(obj);
  cc_scratch_end();
  return 0;
}
