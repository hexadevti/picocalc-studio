// pcx_load.c - .pcx module loader (see pcx.h): parse, relocate, line lookup.
//
// Portable C99: built by the firmware (Arduino ESP32), by the RV32 simulator runner
// (tools/host/simfw/pcxrun.c) and by host gcc. No cache maintenance here: on the device the
// caller writes text_buf to flash and maps it at text_addr afterwards.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pcx.h"

static int fail(char *err, int errlen, const char *msg, const char *arg) {
  if (err && errlen > 0) {
    if (arg) snprintf(err, (size_t)errlen, msg, arg);
    else snprintf(err, (size_t)errlen, "%s", msg);
  }
  return -1;
}

// Reserves n entries of `size` bytes at *off (which must stay 4-byte aligned); overflow safe.
static int take(size_t *off, size_t len, uint32_t n, size_t size, size_t *start) {
  if (*off > len || n > (len - *off) / size) return -1;
  *start = *off;
  *off += (size_t)n * size;
  return 0;
}

int pcx_parse(const void *buf, size_t len, PcxView *v, char *err, int errlen) {
  const uint8_t *b = (const uint8_t *)buf;
  const PcxHeader *h = (const PcxHeader *)buf;
  size_t off = sizeof(PcxHeader), text, data, imports, relocs, lines, files, strtab;
  uint32_t i;
  memset(v, 0, sizeof *v);
  if (!buf || ((uintptr_t)buf & 3u)) return fail(err, errlen, "image buffer not 4-byte aligned", NULL);
  if (len < sizeof(PcxHeader)) return fail(err, errlen, "file too short for a .pcx header", NULL);
  if (h->magic != PCX_MAGIC) return fail(err, errlen, "not a .pcx module (bad magic)", NULL);
  if (h->version != PCX_VERSION) return fail(err, errlen, "unsupported .pcx version", NULL);
  if (h->text_size & 3u) return fail(err, errlen, "text size not a multiple of 4", NULL);
  if (!h->ram_align || (h->ram_align & (h->ram_align - 1u)))
    return fail(err, errlen, "ram alignment not a power of 2", NULL);
  if (h->bss_size > 0xffffffffu - h->data_size || h->data_size + h->bss_size > 0x0fffffffu)
    return fail(err, errlen, "ram block too large", NULL);
  if (take(&off, len, h->text_size, 1, &text)) goto trunc;
  if (take(&off, len, h->data_size, 1, &data)) goto trunc;
  off = (off + 3u) & ~(size_t)3u;  // data is padded to 4
  if (take(&off, len, h->n_imports, 4, &imports)) goto trunc;
  if (take(&off, len, h->n_relocs, sizeof(PcxReloc), &relocs)) goto trunc;
  if (take(&off, len, h->n_lines, sizeof(PcxLine), &lines)) goto trunc;
  if (take(&off, len, h->n_files, 4, &files)) goto trunc;
  if (take(&off, len, h->strtab_size, 1, &strtab)) goto trunc;

  v->h = h;
  v->text = b + text;
  v->data = b + data;
  v->imports = (const uint32_t *)(const void *)(b + imports);
  v->relocs = (const PcxReloc *)(const void *)(b + relocs);
  v->lines = (const PcxLine *)(const void *)(b + lines);
  v->files = (const uint32_t *)(const void *)(b + files);
  v->strtab = (const char *)(b + strtab);

  if ((h->entry_main != PCX_NO_ENTRY && h->entry_main >= h->text_size) ||
      (h->entry_setup != PCX_NO_ENTRY && h->entry_setup >= h->text_size) ||
      (h->entry_loop != PCX_NO_ENTRY && h->entry_loop >= h->text_size))
    return fail(err, errlen, "entry point outside the text", NULL);
  if (h->strtab_size && v->strtab[h->strtab_size - 1])
    return fail(err, errlen, "string table not NUL-terminated", NULL);
  for (i = 0; i < h->n_imports; i++)
    if (v->imports[i] >= h->strtab_size) return fail(err, errlen, "import name outside the string table", NULL);
  for (i = 0; i < h->n_files; i++)
    if (v->files[i] >= h->strtab_size) return fail(err, errlen, "file name outside the string table", NULL);
  for (i = 0; i < h->n_lines; i++)
    if (v->lines[i].file >= h->n_files || v->lines[i].text_off >= h->text_size ||
        (i && v->lines[i].text_off < v->lines[i - 1].text_off))
      return fail(err, errlen, "bad line table", NULL);
  return 0;
trunc:
  memset(v, 0, sizeof *v);
  return fail(err, errlen, "truncated .pcx (tables extend past the end of the file)", NULL);
}

static uint32_t rd32(const uint8_t *p) {
  uint32_t x;
  memcpy(&x, p, 4);  // native little endian on every build (host x86, RV32)
  return x;
}

static void wr32(uint8_t *p, uint32_t x) { memcpy(p, &x, 4); }

// Patches `auipc rd, 0; <I- or S-type using rd>` at p (executing at pc) to reach target.
static int patch_pair(uint8_t *p, uint32_t pc, uint32_t target, char *err, int errlen) {
  uint32_t hi_insn = rd32(p), lo_insn = rd32(p + 4);
  uint32_t pcrel = target - pc;
  uint32_t hi = (pcrel + 0x800u) & 0xfffff000u;  // rounds so that lo fits in signed 12 bits
  uint32_t lo = (pcrel - hi) & 0xfffu;
  uint32_t op = lo_insn & 0x7fu;
  if ((hi_insn & 0x7fu) != 0x17u) return fail(err, errlen, "PAIR relocation not at an auipc", NULL);
  if (((lo_insn >> 15) & 31u) != ((hi_insn >> 7) & 31u))
    return fail(err, errlen, "PAIR relocation: second instruction does not use the auipc register", NULL);
  hi_insn = (hi_insn & 0xfffu) | hi;
  if (op == 0x13u || op == 0x03u || op == 0x07u || op == 0x67u) {          // addi, loads, jalr
    if (op == 0x13u && ((lo_insn >> 12) & 7u) != 0)
      return fail(err, errlen, "PAIR relocation: OP-IMM other than addi", NULL);
    lo_insn = (lo_insn & 0x000fffffu) | lo << 20;
  } else if (op == 0x23u || op == 0x27u) {                                 // stores
    lo_insn = (lo_insn & 0x01fff07fu) | (lo >> 5) << 25 | (lo & 31u) << 7;
  } else {
    return fail(err, errlen, "PAIR relocation: unsupported second instruction", NULL);
  }
  wr32(p, hi_insn);
  wr32(p + 4, lo_insn);
  return 0;
}

int pcx_relocate(const PcxView *v, uint8_t *text_buf, uint32_t text_addr, uint8_t *ram,
                 void *(*resolve)(const char *name), char *err, int errlen) {
  const PcxHeader *h = v->h;
  uint32_t i, ram_addr = (uint32_t)(uintptr_t)ram;
  if (h->data_size) memcpy(ram, v->data, h->data_size);
  if (h->bss_size) memset(ram + h->data_size, 0, h->bss_size);

  // Check every import up front, so an undefined symbol is reported even without a use.
  for (i = 0; i < h->n_imports; i++) {
    const char *name = v->strtab + v->imports[i];
    if (!resolve(name)) return fail(err, errlen, "undefined symbol '%s'", name);
  }

  for (i = 0; i < h->n_relocs; i++) {
    const PcxReloc *r = &v->relocs[i];
    uint32_t off = PCX_W_OFFSET(r->where), type = PCX_W_TYPE(r->where), idx = PCX_T_INDEX(r->target);
    int in_ram = (r->where & PCX_W_RAM) != 0;
    uint32_t size = in_ram ? h->data_size : h->text_size;
    uint32_t need = type == PCX_R_PAIR ? 8u : 4u;
    uint32_t target;
    uint8_t *p;
    switch (PCX_T_KIND(r->target)) {
    case PCX_T_TEXT: target = text_addr; break;
    case PCX_T_RAM: target = ram_addr; break;
    case PCX_T_IMPORT:
      if (idx >= h->n_imports) return fail(err, errlen, "relocation names a missing import", NULL);
      target = (uint32_t)(uintptr_t)resolve(v->strtab + v->imports[idx]);
      break;
    default: return fail(err, errlen, "relocation with an unknown target kind", NULL);
    }
    target += (uint32_t)r->addend;
    if (type > PCX_R_ABS32) return fail(err, errlen, "relocation with an unknown type", NULL);
    if (off > size || size - off < need) return fail(err, errlen, "relocation outside its block", NULL);
    p = (in_ram ? ram : text_buf) + off;
    if (type == PCX_R_ABS32) {
      wr32(p, target);
    } else {
      if (in_ram) return fail(err, errlen, "PAIR relocation in the ram block", NULL);
      if (patch_pair(p, text_addr + off, target, err, errlen)) return -1;
    }
  }
  return 0;
}

int pcx_lookup_line(const PcxView *v, uint32_t text_off, const char **file, int *line) {
  uint32_t lo = 0, hi = v->h ? v->h->n_lines : 0;
  const PcxLine *l;
  while (lo < hi) {  // first entry with text_off > off
    uint32_t mid = lo + (hi - lo) / 2;
    if (v->lines[mid].text_off <= text_off) lo = mid + 1;
    else hi = mid;
  }
  if (!lo) return 0;
  l = &v->lines[lo - 1];
  if (file) *file = v->strtab + v->files[l->file];
  if (line) *line = l->line;
  return 1;
}
