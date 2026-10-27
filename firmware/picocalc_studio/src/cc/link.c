// link.c - links CcObjects into a .pcx module (see rt/pcx.h).
//
// text = every object's text, in order. ram = all rodata, then all data (the initialized part,
// data_size bytes), then all bss and the merged common symbols. Text-to-text references are
// resolved here; references into ram or to firmware exports become load-time relocations.
// Debug builds (cc_debug): the stop tables are joined, and __dbg_flags (one byte per stop point,
// at the end of bss) is defined here; an object's stop i refers to its byte as __dbg_flags + i.
#include "chibicc.h"
#include "asm_rv32.h"
#include "../rt/pcx.h"

typedef struct {
  int obj, sym;  // definition, or obj = -1 if only common
  uint32_t common_size, common_align;
} GlobalDef;

typedef struct {
  int kind;  // PCX_T_*
  uint32_t index_or_off;
} Target;

static CcObject **objs;
static int nobjs;
static uint32_t (*sect_base)[CC_NSECT];  // per object
static HashMap globals;      // name -> GlobalDef*
static HashMap import_map;   // name -> index + 1
static char **imports;
static int nimports, capimports;

// Growable byte buffer (arena)
typedef struct { uint8_t *data; uint32_t size, cap; } Buf;

static void buf_put(Buf *b, const void *p, uint32_t n) {
  if (b->size + n > b->cap) {
    uint32_t cap = b->cap ? b->cap * 2 : 1024;
    while (cap < b->size + n)
      cap *= 2;
    b->data = cc_realloc(b->data, b->cap, cap);
    b->cap = cap;
  }
  memcpy(b->data + b->size, p, n);
  b->size += n;
}

static void buf_u32(Buf *b, uint32_t v) { buf_put(b, &v, 4); }

static noreturn void link_error(CcObject *o, uint32_t text_off, int line, char *fmt, ...) {
  // Prefer the line table: it knows the file (the reference may be in a header).
  char *file = o ? o->path : NULL;
  if (o && text_off != UINT32_MAX && o->nlines) {
    int lo = 0, hi = o->nlines - 1, found = -1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (o->lines[mid].text_off <= text_off) {
        found = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
    if (found >= 0) {
      file = o->files[o->lines[found].file];
      line = o->lines[found].line;
    }
  }
  int n = 0;
  if (file && line > 0)
    n = snprintf(cc_errbuf, sizeof(cc_errbuf), "%s:%d: error: ", file, line);
  else if (file)
    n = snprintf(cc_errbuf, sizeof(cc_errbuf), "%s: error: ", file);
  else
    n = snprintf(cc_errbuf, sizeof(cc_errbuf), "error: ");
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(cc_errbuf + n, sizeof(cc_errbuf) - n, fmt, ap);
  va_end(ap);
  longjmp(cc_error_jmp, 1);
}

static int import_index(char *name) {
  intptr_t i = (intptr_t)hashmap_get(&import_map, name);
  if (i)
    return i - 1;
  if (nimports == capimports) {
    int cap = capimports ? capimports * 2 : 32;
    imports = cc_realloc(imports, sizeof(char *) * capimports, sizeof(char *) * cap);
    capimports = cap;
  }
  imports[nimports] = name;
  hashmap_put(&import_map, name, (void *)(intptr_t)(nimports + 1));
  return nimports++;
}

static Target sym_target(int oi, CcSym *s) {
  int sect = s->sect;
  uint32_t off = s->offset;
  if (sect < 0 || s->is_common) {
    GlobalDef *g = hashmap_get(&globals, s->name);
    if (!g)
      return (Target){-1, 0};
    if (g->obj < 0)
      return (Target){PCX_T_RAM, g->common_size};  // common_size holds its ram offset after layout
    CcSym *d = &objs[g->obj]->syms[g->sym];
    oi = g->obj;
    sect = d->sect;
    off = d->offset;
  }
  if (sect == CC_TEXT)
    return (Target){PCX_T_TEXT, sect_base[oi][CC_TEXT] + off};
  return (Target){PCX_T_RAM, sect_base[oi][sect] + off};
}

static GlobalDef *find_entry(char *name) {
  GlobalDef *g = hashmap_get(&globals, name);
  if (g && g->obj >= 0 && objs[g->obj]->syms[g->sym].sect == CC_TEXT)
    return g;
  return NULL;
}

static uint32_t entry_off(char *name) {
  GlobalDef *g = find_entry(name);
  if (!g)
    return PCX_NO_ENTRY;
  return sect_base[g->obj][CC_TEXT] + objs[g->obj]->syms[g->sym].offset;
}

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

int cc_link(CcObject **objv, int n, const CcOptions *opt, uint8_t **image, size_t *size) {
  *image = NULL;
  *size = 0;
  cc_errbuf[0] = '\0';
  if (setjmp(cc_error_jmp))
    return -1;

  objs = objv;
  nobjs = n;
  globals = import_map = (HashMap){0};
  imports = NULL;
  nimports = capimports = 0;
  sect_base = calloc(n ? n : 1, sizeof(*sect_base));

  // 1. Global symbol table
  for (int i = 0; i < n; i++) {
    CcObject *o = objs[i];
    for (int j = 0; j < o->nsyms; j++) {
      CcSym *s = &o->syms[j];
      if (s->sect < 0 || !s->is_global)
        continue;
      GlobalDef *g = hashmap_get(&globals, s->name);
      if (!g) {
        g = calloc(1, sizeof(GlobalDef));
        g->obj = -1;
        hashmap_put(&globals, s->name, g);
      }
      if (s->is_common) {
        if (s->size > g->common_size)
          g->common_size = s->size;
        if (s->offset > g->common_align)
          g->common_align = s->offset;
        continue;
      }
      if (g->obj >= 0)
        link_error(o, UINT32_MAX, s->line, "multiple definition of '%s'", s->name);
      g->obj = i;
      g->sym = j;
    }
  }

  // 2. Layout
  uint32_t text_size = 0, ram = 0, ram_align = 4;
  for (int i = 0; i < n; i++) {
    text_size = align_up(text_size, 4);
    sect_base[i][CC_TEXT] = text_size;
    text_size += objs[i]->sect[CC_TEXT].size;
  }
  text_size = align_up(text_size, 4);
  for (int sect = CC_RODATA; sect <= CC_BSS; sect++) {
    if (sect == CC_BSS)
      ram = align_up(ram, 4);  // end of the initialized part
    for (int i = 0; i < n; i++) {
      CcSection *s = &objs[i]->sect[sect];
      uint32_t a = s->align ? s->align : 4;
      if (a > ram_align)
        ram_align = a;
      ram = align_up(ram, a);
      sect_base[i][sect] = ram;
      ram += s->size;
    }
    if (sect == CC_DATA)
      ram = align_up(ram, 4);
  }
  uint32_t data_size = 0;
  for (int i = 0; i < n; i++) {
    uint32_t end = sect_base[i][CC_DATA] + objs[i]->sect[CC_DATA].size;
    if (end > data_size)
      data_size = end;
  }
  data_size = align_up(data_size, 4);
  if (ram < data_size)
    ram = data_size;

  // Commons that no object defined: allocate in bss (common_size becomes the ram offset).
  for (int i = 0; i < n; i++) {
    CcObject *o = objs[i];
    for (int j = 0; j < o->nsyms; j++) {
      CcSym *s = &o->syms[j];
      if (!s->is_common)
        continue;
      GlobalDef *g = hashmap_get(&globals, s->name);
      if (!g || g->obj >= 0 || g->common_align == UINT32_MAX)
        continue;
      uint32_t a = g->common_align ? g->common_align : 4;
      if (a > ram_align)
        ram_align = a;
      ram = align_up(ram, a);
      uint32_t sz = g->common_size;
      g->common_size = ram;
      g->common_align = UINT32_MAX;  // placed
      ram += sz;
    }
  }
  uint32_t nstops = 0, flags_off = ram;
  uint32_t *stop_base = calloc(n ? n : 1, sizeof(uint32_t));
  for (int i = 0; i < n; i++) {
    stop_base[i] = nstops;
    nstops += objs[i]->nstops;
  }
  ram += nstops;
  ram = align_up(ram, 4);

  // 3. Entries
  if (!find_entry("main") && !find_entry("setup"))
    link_error(NULL, UINT32_MAX, 0, "no entry point: define main() or setup() and loop()");

  // 4. Text and data images, relocations
  uint8_t *text = calloc(1, text_size ? text_size : 4);
  uint8_t *data = calloc(1, data_size ? data_size : 4);
  for (int i = 0; i < n; i++) {
    CcObject *o = objs[i];
    CcSection *t = &o->sect[CC_TEXT];
    uint8_t *p = text + sect_base[i][CC_TEXT];
    for (int k = 0; k < t->nparts; p += t->parts[k++].size)
      memcpy(p, t->parts[k].data, t->parts[k].size);
    if (t->size > t->base)
      memcpy(p, t->data, t->size - t->base);
    for (int sect = CC_RODATA; sect <= CC_DATA; sect++)
      if (o->sect[sect].size)
        memcpy(data + sect_base[i][sect], o->sect[sect].data, o->sect[sect].size);
  }

  Buf relocs = {0};
  int nrelocs = 0;
  for (int i = 0; i < n; i++) {
    CcObject *o = objs[i];
    for (int r = 0; r < o->nrelocs; r++) {
      CcReloc *rel = &o->relocs[r / CC_RELOC_BLOCK][r % CC_RELOC_BLOCK];
      CcSym *s = &o->syms[rel->sym];
      Target t = sym_target(i, s);
      if (t.kind < 0 && nstops && !strcmp(s->name, "__dbg_flags"))
        t = (Target){PCX_T_RAM, flags_off + stop_base[i]};
      if (t.kind < 0) {
        if (!opt->have_import || !opt->have_import(s->name))
          link_error(o, rel->sect == CC_TEXT ? rel->offset : UINT32_MAX, rel->line,
                     "undefined reference to '%s'", s->name);
        t = (Target){PCX_T_IMPORT, import_index(s->name)};
      }

      uint32_t where = sect_base[i][rel->sect] + rel->offset;
      if (rel->sect == CC_TEXT && rel->type == CC_R_PAIR && t.kind == PCX_T_TEXT) {
        rv_patch_pair(text + where, (int32_t)(t.index_or_off + rel->addend - where));
        continue;
      }
      PcxReloc pr;
      pr.where = (rel->sect == CC_TEXT ? 0 : PCX_W_RAM) | (uint32_t)rel->type << 28 | where;
      if (t.kind == PCX_T_IMPORT) {
        pr.target = PCX_T_IMPORT << 24 | t.index_or_off;
        pr.addend = rel->addend;
      } else {
        pr.target = (uint32_t)t.kind << 24;
        pr.addend = (int32_t)t.index_or_off + rel->addend;
      }
      buf_put(&relocs, &pr, sizeof(pr));
      nrelocs++;
    }
  }

  // 5. Line and file tables
  Buf lines = {0}, strtab = {0};
  HashMap file_map = {0};
  Buf files = {0};
  int nfiles = 0, nlines = 0;
  buf_put(&strtab, "", 1);
  for (int i = 0; i < n; i++) {
    CcObject *o = objs[i];
    for (int l = 0; l < o->nlines; l++) {
      CcLine *cl = &o->lines[l];
      char *name = o->files[cl->file];
      intptr_t f = (intptr_t)hashmap_get(&file_map, name);
      if (!f) {
        buf_u32(&files, strtab.size);
        buf_put(&strtab, name, strlen(name) + 1);
        f = ++nfiles;
        hashmap_put(&file_map, name, (void *)f);
      }
      PcxLine pl = {sect_base[i][CC_TEXT] + cl->text_off, (uint16_t)(f - 1),
                    (uint16_t)(cl->line > 65535 ? 65535 : cl->line)};
      buf_put(&lines, &pl, sizeof(pl));
      nlines++;
    }
  }
  Buf imp = {0};
  for (int i = 0; i < nimports; i++) {
    buf_u32(&imp, strtab.size);
    buf_put(&strtab, imports[i], strlen(imports[i]) + 1);
  }

  Buf dbg = {0};
  if (nstops) {
    uint32_t nfuncs = 0;
    for (int i = 0; i < n; i++)
      nfuncs += objs[i]->nfuncs;
    PcxDebug d = {PCX_DEBUG_MAGIC, nstops, flags_off, nfuncs};
    buf_put(&dbg, &d, sizeof(d));
    for (int i = 0; i < n; i++)
      for (int k = 0; k < objs[i]->nstops; k++)
        buf_u32(&dbg, sect_base[i][CC_TEXT] + objs[i]->stops[k]);
    for (int i = 0; i < n; i++)
      for (int k = 0; k < objs[i]->nfuncs; k++) {
        CcFunc *f = &objs[i]->funcs[k];
        PcxFunc pf = {sect_base[i][CC_TEXT] + f->lo, sect_base[i][CC_TEXT] + f->hi, strtab.size,
                      f->va, f->save_x, f->save_f};
        buf_put(&strtab, f->name, strlen(f->name) + 1);
        buf_put(&dbg, &pf, sizeof(pf));
      }
  }

  // 6. Output
  PcxHeader h = {0};
  h.magic = PCX_MAGIC;
  h.version = PCX_VERSION;
  h.text_size = text_size;
  h.data_size = data_size;
  h.bss_size = ram - data_size;
  h.ram_align = ram_align;
  h.n_imports = nimports;
  h.n_relocs = nrelocs;
  h.n_lines = nlines;
  h.n_files = nfiles;
  h.strtab_size = strtab.size;
  h.entry_main = entry_off("main");
  h.entry_setup = entry_off("setup");
  h.entry_loop = entry_off("loop");

  size_t total = sizeof(h) + text_size + data_size + imp.size + relocs.size + lines.size +
                 files.size + strtab.size;
  uint32_t pad = (4 - total % 4) % 4;  // the debug trailer is 4-byte aligned
  if (dbg.size)
    total += pad + dbg.size;
  uint8_t *outp = cc_image_alloc(total);
  if (!outp)
    cc_out_of_memory("image", total);
  uint8_t *p = outp;
#define PUT(src, len) (memcpy(p, (src), (len)), p += (len))
  PUT(&h, sizeof(h));
  PUT(text, text_size);
  PUT(data, data_size);
  PUT(imp.data, imp.size);
  PUT(relocs.data, relocs.size);
  PUT(lines.data, lines.size);
  PUT(files.data, files.size);
  PUT(strtab.data, strtab.size);
  if (dbg.size) {
    memset(p, 0, pad);
    p += pad;
    PUT(dbg.data, dbg.size);
  }
#undef PUT
  *image = outp;
  *size = total;
  return 0;
}
