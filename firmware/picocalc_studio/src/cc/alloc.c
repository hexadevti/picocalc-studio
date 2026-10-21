// alloc.c - compiler arena, diagnostics, small host helpers and the cc_compile() driver.
#include "chibicc.h"
#ifdef ESP_PLATFORM
# include "esp_heap_caps.h"
#endif

//
// Arena: bump allocation in 64 KB chunks; big requests get a chunk of their own.
//

#define CHUNK_SIZE (64 * 1024)

typedef struct Chunk Chunk;
struct Chunk {
  Chunk *next;
  size_t size, used;
  _Alignas(8) unsigned char data[];
};

static Chunk *chunks;
static size_t arena_used;
static void *last_alloc;  // most recent allocation, which cc_realloc can grow in place

static void *raw_alloc(size_t n) {
#ifdef ESP_PLATFORM
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
#else
  return malloc(n);
#endif
}

static void *arena_alloc(size_t n) {
  n = (n + 7) & ~(size_t)7;
  if (!chunks || chunks->size - chunks->used < n) {
    size_t size = n > CHUNK_SIZE / 4 ? n : CHUNK_SIZE;
    Chunk *c = raw_alloc(sizeof(Chunk) + size);
    if (!c)
      error("out of memory");
    c->size = size;
    c->used = 0;
    if (chunks && size != CHUNK_SIZE) {
      // Keep allocating from the current chunk; slot the big one in behind it.
      c->used = size;
      c->next = chunks->next;
      chunks->next = c;
      arena_used += n;
      last_alloc = NULL;
      return c->data;
    }
    c->next = chunks;
    chunks = c;
  }
  void *p = chunks->data + chunks->used;
  chunks->used += n;
  arena_used += n;
  last_alloc = p;
  return p;
}

void *cc_calloc(size_t n, size_t size) {
  size_t total = n * size;
  void *p = arena_alloc(total ? total : 1);
  memset(p, 0, total);
  return p;
}

void *cc_realloc(void *p, size_t old_size, size_t new_size) {
  if (p && p == last_alloc) {
    size_t old_r = (old_size + 7) & ~(size_t)7, new_r = (new_size + 7) & ~(size_t)7;
    if (chunks->used - old_r + new_r <= chunks->size) {
      chunks->used = chunks->used - old_r + new_r;
      arena_used = arena_used - old_r + new_r;
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

void cc_arena_free(void) {
  while (chunks) {
    Chunk *next = chunks->next;
    free(chunks);
    chunks = next;
  }
  arena_used = 0;
  last_alloc = NULL;
}

size_t cc_arena_used(void) { return arena_used; }

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

int cc_compile(const CcOptions *opt, const char *path, CcObject **out) {
  *out = NULL;
  cc_errbuf[0] = '\0';
  if (setjmp(cc_error_jmp))
    return -1;

  tokenize_reset();
  preprocess_reset();
  parse_reset();

  include_paths = (StringArray){0};
  for (int i = 0; i < opt->n_include_paths; i++)
    strarray_push(&include_paths, strdup(opt->include_paths[i]));

  char *file = strdup(path);
  base_file = file;
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

  Token *tok = tokenize_file(file);
  if (!tok)
    error("cannot open %s", file);
  tok = preprocess(tok);
  Obj *prog = parse(tok);

  CcObject *obj = calloc(1, sizeof(CcObject));
  obj->path = file;
  codegen(prog, obj);
  *out = obj;
  return 0;
}
