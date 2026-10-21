// asm_rv32.c - RV32 instruction encoders, labels and fixups.
#include "chibicc.h"
#include "asm_rv32.h"

static CcObject *obj;
static CcSection *text;

// Label positions (-1 = unbound) and pending references for the current function.
enum { FX_JAL, FX_BRANCH, FX_PAIR };
typedef struct { uint32_t off; int label; int kind; } Fixup;
static int32_t *labels;
static int nlabels, caplabels;
static Fixup *fixups;
static int nfixups, capfixups;

void sect_append(CcSection *s, const void *data, uint32_t n) {
  if (s->size + n > s->cap) {
    uint32_t cap = s->cap ? s->cap : 256;
    while (cap < s->size + n)
      cap *= 2;
    s->data = cc_realloc(s->data, s->cap, cap);
    s->cap = cap;
  }
  if (data)
    memcpy(s->data + s->size, data, n);
  else
    memset(s->data + s->size, 0, n);
  s->size += n;
}

void sect_align(CcSection *s, uint32_t align) {
  if (align > s->align)
    s->align = align;
  uint32_t pad = (align - s->size % align) % align;
  if (pad)
    sect_append(s, NULL, pad);
}

void asm_init(CcObject *o) {
  obj = o;
  text = &o->sect[CC_TEXT];
  text->align = 4;
  labels = NULL;
  nlabels = caplabels = 0;
  fixups = NULL;
  nfixups = capfixups = 0;
}

uint32_t asm_pc(void) { return text->size; }

void emit32(uint32_t insn) { sect_append(text, &insn, 4); }

static uint32_t read32(uint32_t off) {
  uint32_t v;
  memcpy(&v, text->data + off, 4);
  return v;
}

static void write32(uint32_t off, uint32_t v) { memcpy(text->data + off, &v, 4); }

int asm_new_label(void) {
  if (nlabels == caplabels) {
    int cap = caplabels ? caplabels * 2 : 64;
    labels = cc_realloc(labels, sizeof(*labels) * caplabels, sizeof(*labels) * cap);
    caplabels = cap;
  }
  labels[nlabels] = -1;
  return nlabels++;
}

void asm_bind(int label) { labels[label] = text->size; }

static void add_fixup(int label, int kind) {
  if (nfixups == capfixups) {
    int cap = capfixups ? capfixups * 2 : 64;
    fixups = cc_realloc(fixups, sizeof(*fixups) * capfixups, sizeof(*fixups) * cap);
    capfixups = cap;
  }
  fixups[nfixups++] = (Fixup){text->size, label, kind};
}

static uint32_t enc_j(int32_t imm) {
  return ((imm >> 20) & 1) << 31 | ((imm >> 1) & 0x3ff) << 21 | ((imm >> 11) & 1) << 20 |
         ((imm >> 12) & 0xff) << 12;
}

static uint32_t enc_b(int32_t imm) {
  return ((imm >> 12) & 1) << 31 | ((imm >> 5) & 0x3f) << 25 | ((imm >> 1) & 0xf) << 8 |
         ((imm >> 11) & 1) << 7;
}

void asm_end_function(void) {
  for (int i = 0; i < nfixups; i++) {
    Fixup *f = &fixups[i];
    if (labels[f->label] < 0)
      error("internal error: unbound label");
    int32_t d = labels[f->label] - (int32_t)f->off;
    uint32_t insn = read32(f->off);
    switch (f->kind) {
    case FX_JAL:
      if (d < -(1 << 20) || d >= (1 << 20))
        error("function too large (jump out of range)");
      write32(f->off, (insn & 0xfff) | enc_j(d));
      break;
    case FX_BRANCH:
      write32(f->off, (insn & 0x01fff07f) | enc_b(d));
      break;
    case FX_PAIR:
      rv_patch_pair(text->data + f->off, d);
      break;
    }
  }
  nlabels = nfixups = 0;
}

//
// Encoders
//

void rv_r(int f7, int rs2, int rs1, int f3, int rd, int op) {
  emit32((uint32_t)f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op);
}

void rv_i(int imm, int rs1, int f3, int rd, int op) {
  emit32((uint32_t)(imm & 0xfff) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op);
}

void rv_s(int imm, int rs2, int rs1, int f3, int op) {
  emit32((uint32_t)((imm >> 5) & 0x7f) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 |
         (imm & 0x1f) << 7 | op);
}

static int fits12(int32_t v) { return v >= -2048 && v <= 2047; }

void rv_addi(int rd, int rs, int imm) { rv_i(imm, rs, 0, rd, OP_IMM); }

void rv_li(int rd, int32_t imm) {
  if (fits12(imm)) {
    rv_addi(rd, ZERO, imm);
    return;
  }
  uint32_t hi = ((uint32_t)imm + 0x800) & 0xfffff000u;
  int32_t lo = (int32_t)((uint32_t)imm - hi);
  emit32(hi | rd << 7 | OP_LUI);
  if (lo)
    rv_addi(rd, rd, lo);
}

void rv_mv(int rd, int rs) {
  if (rd != rs)
    rv_addi(rd, rs, 0);
}

void rv_op(int f7, int f3, int rd, int a, int b) { rv_r(f7, b, a, f3, rd, OP_OP); }
void rv_add(int rd, int a, int b) { rv_op(0, 0, rd, a, b); }
void rv_sub(int rd, int a, int b) { rv_op(0x20, 0, rd, a, b); }
void rv_opi(int f3, int rd, int rs, int imm) { rv_i(imm, rs, f3, rd, OP_IMM); }
void rv_shi(int f3, int f7, int rd, int rs, int sh) { rv_i((f7 << 5) | (sh & 31), rs, f3, rd, OP_IMM); }

void rv_addr(int rd, int base, int32_t off) {
  if (fits12(off)) {
    rv_addi(rd, base, off);
  } else {
    rv_li(T5, off);
    rv_add(rd, base, T5);
  }
}

// Returns the base register to use with a 12-bit offset *off (materializing into T5 if needed).
static int mem_base(int base, int32_t *off) {
  if (fits12(*off))
    return base;
  uint32_t hi = ((uint32_t)*off + 0x800) & 0xfffff000u;
  emit32(hi | T5 << 7 | OP_LUI);
  rv_add(T5, T5, base);
  *off = (int32_t)((uint32_t)*off - hi);
  return T5;
}

void rv_load(int rd, int base, int32_t off, int width, int is_unsigned) {
  int f3 = width == 1 ? 0 : width == 2 ? 1 : 2;
  if (is_unsigned && width < 4)
    f3 |= 4;
  base = mem_base(base, &off);
  rv_i(off, base, f3, rd, OP_LOAD);
}

void rv_store(int rs, int base, int32_t off, int width) {
  base = mem_base(base, &off);
  rv_s(off, rs, base, width == 1 ? 0 : width == 2 ? 1 : 2, OP_STORE);
}

void rv_flw(int fd, int base, int32_t off) {
  base = mem_base(base, &off);
  rv_i(off, base, 2, fd, OP_LOADFP);
}

void rv_fsw(int fs, int base, int32_t off) {
  base = mem_base(base, &off);
  rv_s(off, fs, base, 2, OP_STOREFP);
}

void rv_fp(int f7, int rs2, int rs1, int rm, int rd) { rv_r(f7, rs2, rs1, rm, rd, OP_FP); }

void rv_branch(int cond, int a, int b, int label) {
  int32_t pos = labels[label];
  if (pos >= 0) {
    int32_t d = pos - (int32_t)text->size;
    if (d >= -4096) {
      emit32(enc_b(d) | b << 20 | a << 15 | cond << 12 | OP_BRANCH);
      return;
    }
  }
  // Inverted short branch over a jal: reaches +-1 MB.
  emit32(enc_b(8) | b << 20 | a << 15 | (cond ^ 1) << 12 | OP_BRANCH);
  rv_jump(label);
}

void rv_jump(int label) {
  add_fixup(label, FX_JAL);
  emit32(OP_JAL);  // jal zero, label
}

void rv_label_addr(int rd, int label) {
  add_fixup(label, FX_PAIR);
  emit32(rd << 7 | OP_AUIPC);
  rv_addi(rd, rd, 0);
}

static void add_reloc(int sym, int32_t addend, int line) {
  if (obj->nrelocs == obj->caprelocs) {
    int cap = obj->caprelocs ? obj->caprelocs * 2 : 64;
    obj->relocs = cc_realloc(obj->relocs, sizeof(CcReloc) * obj->caprelocs, sizeof(CcReloc) * cap);
    obj->caprelocs = cap;
  }
  obj->relocs[obj->nrelocs++] = (CcReloc){CC_TEXT, text->size, CC_R_PAIR, sym, addend, line};
}

void rv_sym_addr(int rd, int sym, int32_t addend, int line) {
  add_reloc(sym, addend, line);
  emit32(rd << 7 | OP_AUIPC);
  rv_addi(rd, rd, 0);
}

void rv_call_sym(int sym, int line) {
  add_reloc(sym, 0, line);
  emit32(RA << 7 | OP_AUIPC);
  rv_i(0, RA, 0, RA, OP_JALR);
}

void rv_patch_pair(uint8_t *p, int32_t pcrel) {
  uint32_t hi = ((uint32_t)pcrel + 0x800) & 0xfffff000u;
  uint32_t lo = (uint32_t)pcrel - hi;
  uint32_t a, b;
  memcpy(&a, p, 4);
  memcpy(&b, p + 4, 4);
  a = (a & 0xfff) | hi;
  uint32_t op = b & 0x7f;
  if (op == OP_STORE || op == OP_STOREFP)
    b = (b & 0x01fff07f) | ((lo >> 5) & 0x7f) << 25 | (lo & 0x1f) << 7;
  else
    b = (b & 0x000fffff) | (lo & 0xfff) << 20;
  memcpy(p, &a, 4);
  memcpy(p + 4, &b, 4);
}
