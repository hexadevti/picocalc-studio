// alloc.c - compiler arena, diagnostics, small host helpers and the cc_compile() driver.
#include "chibicc.h"
#ifdef ESP_PLATFORM
# include "esp_heap_caps.h"
#endif

//
// Arena: bump allocation in 16 KB chunks; big requests get a chunk of their own.
//
// There are four. The object arena holds finished CcObjects until cc_arena_free(). The main
// arena holds one compile (tokens, AST, codegen buffers) and is dropped once its object has been
// copied out. The scratch arena holds the preprocessor's working set (raw tokens, macro bodies,
// expansion copies) and is dropped as soon as the surviving tokens have been copied out
// (cc_scratch_begin/end, keep_token()). The function arena holds one function's Nodes and locals
// while it is parsed and compiled (cc_fn_begin/end).
//

#define CHUNK_SIZE (16 * 1024)

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

static Arena obj_arena, main_arena, scratch_arena, fn_arena;
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
    Chunk *c = raw_alloc(sizeof(Chunk) + size);
    if (!c)
      error("out of memory");
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
    arena_reserved -= sizeof(Chunk) + a->chunks->size;
    free(a->chunks);
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
  Grown *h = raw_realloc(g, sizeof(Grown) + new_size);
  if (!h)
    error("out of memory");
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
  free(g);
}

static void free_grown(void) {
  tracked = NULL;
  ntracked = tracked_cap = 0;
  tracking = false;
  while (grown.next != &grown) {
    Grown *g = grown.next;
    grown.next = g->next;
    arena_reserved -= sizeof(Grown) + g->size;
    free(g);
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

// What the function arena holds now; cc_fn_release() drops what is allocated after it.
CcFnMark cc_fn_mark(void) {
  Chunk *c = fn_arena.chunks;
  return (CcFnMark){c, c ? c->next : NULL, c ? c->used : 0, fn_arena.used};
}

void cc_fn_release(CcFnMark m) {
  Arena *a = &fn_arena;
  Chunk *top = m.chunk;
  // Chunks started since are in front of the marked one; big ones may sit right behind it.
  while (a->chunks != top) {
    Chunk *next = a->chunks->next;
    arena_reserved -= sizeof(Chunk) + a->chunks->size;
    free(a->chunks);
    a->chunks = next;
  }
  if (top) {
    while (top->next != m.next) {
      Chunk *c = top->next;
      top->next = c->next;
      arena_reserved -= sizeof(Chunk) + c->size;
      free(c);
    }
    top->used = m.used;
  }
  a->used = m.total;
  a->last_alloc = NULL;
}

void cc_fn_end(void) {
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

bool cc_keep(bool on) {
  bool was = arena == &main_arena;
  arena = on || !scratch_open ? &main_arena : &scratch_arena;
  return was;
}

void cc_arena_free(void) {
  cc_scratch_end();
  cc_fn_end();
  free_arena(&main_arena);
  free_grown();
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

//
// Driver
//

static void *dup_mem(const void *p, size_t n) {
  if (!p)
    return NULL;
  void *q = arena_alloc(n ? n : 1);
  memcpy(q, p, n);
  return q;
}

// Copies a finished object out of the main arena, which can then be dropped: linking needs
// only the objects, and the next file's compile can reuse the memory.
static CcObject *keep_object(CcObject *o) {
  arena = &obj_arena;
  CcObject *k = cc_calloc(1, sizeof(CcObject));
  k->path = cc_strdup(o->path);
  for (int i = 0; i < CC_NSECT; i++) {
    k->sect[i] = o->sect[i];
    k->sect[i].data = dup_mem(o->sect[i].data, o->sect[i].size);
    k->sect[i].cap = o->sect[i].size;
  }
  k->syms = dup_mem(o->syms, sizeof(CcSym) * o->nsyms);
  k->nsyms = k->capsyms = o->nsyms;
  for (int i = 0; i < o->nsyms; i++)
    k->syms[i].name = cc_strdup(o->syms[i].name);
  k->relocs = dup_mem(o->relocs, sizeof(CcReloc) * o->nrelocs);
  k->nrelocs = k->caprelocs = o->nrelocs;
  k->lines = dup_mem(o->lines, sizeof(CcLine) * o->nlines);
  k->nlines = k->caplines = o->nlines;
  k->files = dup_mem(o->files, sizeof(char *) * o->nfiles);
  k->nfiles = k->capfiles = o->nfiles;
  for (int i = 0; i < o->nfiles; i++)
    k->files[i] = cc_strdup(o->files[i]);
  arena = &main_arena;
  free_arena(&main_arena);
  free_grown();
  return k;
}

int cc_compile(const CcOptions *opt, const char *path, CcObject **out) {
  *out = NULL;
  cc_errbuf[0] = '\0';
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
