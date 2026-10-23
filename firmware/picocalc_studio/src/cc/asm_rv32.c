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

// Two-pass state: the first pass records where each rv_branch was emitted and, at the rollback,
// where every label ended up.
static uint32_t func_start, func_relocs;
static bool second_pass;
static uint32_t *branch_pc;
static int nbranch, capbranch, branch_idx;
static int32_t *pass1_labels;
static int npass1_labels;

static uint32_t fence;       // no instruction before this may be rewritten or removed
static int32_t last_jump = -1;  // offset of the last standalone rv_jump
static int last_jump_label;

void sect_append(CcSection *s, const void *data, uint32_t n) {
  uint32_t used = s->size - s->base;
  if (used + n > s->cap) {
    uint32_t cap = s->cap ? s->cap : 256;
    // Past 8 KB in steps: less slack, and a smaller transient while cc_grow() copies.
    while (cap < used + n)
      cap = cap < 8192 ? cap * 2 : cap + 8192;
    s->data = cc_grow(s->data, s->cap, cap);
    s->cap = cap;
  }
  if (data)
    memcpy(s->data + used, data, n);
  else
    memset(s->data + used, 0, n);
  s->size += n;
}

// Relocations are kept in blocks of up to 8 KB, not one array: growing that would need room for
// the old and the new copy at once.
CcReloc *cc_new_reloc(CcObject *o) {
  int b = o->nrelocs / CC_RELOC_BLOCK, i = o->nrelocs % CC_RELOC_BLOCK;
  if (o->nrelocs == o->caprelocs) {  // block b, if any, is full at i entries
    if (!i) {
      if (!(b & (b - 1))) {  // 0 or a power of 2: the block list is full
        int cap = b ? b * 2 : 4;
        o->relocs = cc_grow(o->relocs, sizeof(CcReloc *) * b, sizeof(CcReloc *) * cap);
      }
      o->relocs[b] = NULL;
    }
    int cap = i ? i * 2 : 64;
    o->relocs[b] = cc_grow(o->relocs[b], sizeof(CcReloc) * i, sizeof(CcReloc) * cap);
    o->caprelocs += cap - i;
  }
  o->nrelocs++;
  return &o->relocs[b][i];
}

// Finishes the bytes written so far as a part of their own, once they are 8 KB: growing one big
// array would need room for the old and the new copy at once.
static void sect_split(CcSection *s) {
  uint32_t used = s->size - s->base;
  if (used < 8192)
    return;
  if (!(s->nparts & (s->nparts - 1))) {  // 0 or a power of 2: full
    int cap = s->nparts ? s->nparts * 2 : 4;
    s->parts = cc_grow(s->parts, sizeof(CcPart) * s->nparts, sizeof(CcPart) * cap);
  }
  s->parts[s->nparts++] = (CcPart){cc_grow_keep(s->data, used), used};
  s->data = NULL;
  s->cap = 0;
  s->base = s->size;
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
  // Arena memory from an earlier compile is gone (the device compiles many times per boot).
  branch_pc = NULL;
  nbranch = capbranch = branch_idx = 0;
  pass1_labels = NULL;
  npass1_labels = 0;
  second_pass = false;
  last_jump = -1;
}

uint32_t asm_pc(void) { return text->size; }

void emit32(uint32_t insn) { sect_append(text, &insn, 4); }

// Code of the current function: at or past text->base.
static uint8_t *text_at(uint32_t off) { return text->data + (off - text->base); }

static uint32_t read32(uint32_t off) {
  uint32_t v;
  memcpy(&v, text_at(off), 4);
  return v;
}

static void write32(uint32_t off, uint32_t v) { memcpy(text_at(off), &v, 4); }

int asm_new_label(void) {
  if (nlabels == caplabels) {
    int cap = caplabels ? caplabels * 2 : 64;
    labels = cc_grow(labels, sizeof(*labels) * caplabels, sizeof(*labels) * cap);
    caplabels = cap;
  }
  labels[nlabels] = -1;
  return nlabels++;
}

void asm_bind(int label) {
  // A jump to the next instruction is dropped.
  if (last_jump >= 0 && last_jump + 4 == (int32_t)text->size && last_jump_label == label &&
      fence <= (uint32_t)last_jump && nfixups && fixups[nfixups - 1].off == (uint32_t)last_jump) {
    text->size -= 4;
    nfixups--;
  }
  last_jump = -1;
  labels[label] = text->size;
  fence = text->size;
}

static void add_fixup(int label, int kind) {
  if (nfixups == capfixups) {
    int cap = capfixups ? capfixups * 2 : 64;
    fixups = cc_grow(fixups, sizeof(*fixups) * capfixups, sizeof(*fixups) * cap);
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

void asm_begin_function(void) {
  sect_split(text);
  func_start = fence = text->size;
  func_relocs = obj->nrelocs;
  second_pass = false;
  nbranch = branch_idx = 0;
  last_jump = -1;
}

void asm_rollback(void) {
  if (npass1_labels < nlabels) {
    pass1_labels = cc_grow(pass1_labels, sizeof(int32_t) * npass1_labels, sizeof(int32_t) * nlabels);
    npass1_labels = nlabels;
  }
  memcpy(pass1_labels, labels, sizeof(int32_t) * nlabels);
  text->size = fence = func_start;
  obj->nrelocs = func_relocs;
  nlabels = nfixups = 0;
  second_pass = true;
  branch_idx = 0;
  last_jump = -1;
}

uint32_t *asm_last_insn(void) {
  if (text->size < fence + 4)
    return NULL;
  return (uint32_t *)text_at(text->size - 4);
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
      if (d < -4096 || d > 4094)
        error("internal error: branch out of range");
      write32(f->off, (insn & 0x01fff07f) | enc_b(d));
      break;
    case FX_PAIR:
      rv_patch_pair(text_at(f->off), d);
      break;
    }
  }
  nlabels = nfixups = 0;
  second_pass = false;
  last_jump = -1;
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

static void emit_jal(int label) {
  add_fixup(label, FX_JAL);
  emit32(OP_JAL);  // jal zero, label
}

void rv_branch(int cond, int a, int b, int label) {
  int k = branch_idx++;
  if (!second_pass) {
    if (nbranch == capbranch) {
      int cap = capbranch ? capbranch * 2 : 256;
      branch_pc = cc_grow(branch_pc, sizeof(*branch_pc) * capbranch, sizeof(*branch_pc) * cap);
      capbranch = cap;
    }
    branch_pc[nbranch++] = text->size;
  }
  int32_t pos = labels[label];
  if (pos >= 0) {
    int32_t d = pos - (int32_t)text->size;
    if (d >= -4096) {
      emit32(enc_b(d) | b << 20 | a << 15 | cond << 12 | OP_BRANCH);
      return;
    }
  } else if (second_pass && k < nbranch && label < npass1_labels && pass1_labels[label] >= 0) {
    // Forward: the distance can only have shrunk since the first pass.
    int32_t d = pass1_labels[label] - (int32_t)branch_pc[k];
    if (d > 0 && d <= 4094) {
      add_fixup(label, FX_BRANCH);
      emit32(b << 20 | a << 15 | cond << 12 | OP_BRANCH);
      return;
    }
  }
  // Inverted short branch over a jal: reaches +-1 MB.
  emit32(enc_b(8) | b << 20 | a << 15 | (cond ^ 1) << 12 | OP_BRANCH);
  emit_jal(label);
}

void rv_jump(int label) {
  last_jump = text->size;
  last_jump_label = label;
  emit_jal(label);
}

void rv_label_addr(int rd, int label) {
  add_fixup(label, FX_PAIR);
  emit32(rd << 7 | OP_AUIPC);
  rv_addi(rd, rd, 0);
}

static void add_reloc(int sym, int32_t addend, int line) {
  *cc_new_reloc(obj) = (CcReloc){.offset = text->size, .addend = addend, .sym = sym, .sect = CC_TEXT, .type = CC_R_PAIR, .line = line};
}

void rv_sym_addr(int rd, int sym, int32_t addend, int line) {
  add_reloc(sym, addend, line);
  emit32(rd << 7 | OP_AUIPC);
  rv_addi(rd, rd, 0);
}

void rv_sym_hi(int rd, int sym, int32_t addend, int line) {
  add_reloc(sym, addend, line);
  emit32(rd << 7 | OP_AUIPC);
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
