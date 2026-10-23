// codegen_rv32.c - RV32IMFAC (ilp32f) code generator: chibicc AST -> machine code in a CcObject.
//
// A stack machine in the style of chibicc's x86-64 backend. The value of the current
// expression lives in the "accumulator":
//   integers <= 32 bits, pointers, struct addresses   a0 (always extended to 32 bits)
//   long long, double (soft-float)                    a0 (low word) : a1 (high word)
//   float                                             fa0
// Binary operators evaluate rhs, push it, evaluate lhs and pop rhs into t0 (+t1) / ft0.
//
// Frame (s0 = sp at entry, 16-byte aligned):
//   s0+0 ...          stack-passed arguments
//   s0-32 .. s0-4     a0..a7 (variadic functions only; contiguous with the stack args)
//   s0-va-4, s0-va-8  saved ra, saved s0
//   below             locals (4-byte aligned slots), then the temporary push area
//
// Calling convention: RISC-V ilp32f, except that structs are never split into FP registers.
// Structs > 8 bytes are passed by reference to a copy; returned via a hidden pointer in a0.
#include "chibicc.h"
#include "asm_rv32.h"

static CcObject *out;
static Obj *current_fn;
static int depth;          // bytes on the temporary push area in memory
static int return_label;

// Callee-saved registers. Local scalars whose address is never taken live in the first ones
// (by use count); the rest hold pushed temporaries (memory when they run out).
static const int sregs[11] = {S1, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
static const int fsregs[12] = {8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
enum { MAX_VAR_REGS = 8 };
static int nvar_x, nvar_f;     // registers taken by variables
static int tmp_x, tmp_f;       // temporaries in use (from the top of the list down)
static uint32_t used_x, used_f;  // registers that must be saved (bit = register number)

// The push stack: where each pushed value is.
typedef struct {
  int8_t reg, reg2;  // register(s), or reg < 0: in memory
  bool fp;
  int mem;           // depth after a memory push
} Slot;
enum { MAX_SLOTS = 512 };
static Slot slots[MAX_SLOTS];
static int nslots;
static HashMap label_map;  // chibicc label names -> asm labels (per function)
static HashMap sym_map;    // symbol name -> index + 1
static HashMap file_map;   // file name -> index + 1
static int cur_line;

static void gen_expr(Node *node);
static void gen_void(Node *node);
static void gen_stmt(Node *node);
static bool const_int(Node *node, int64_t *v);

int align_to(int n, int align) { return (n + align - 1) / align * align; }

//
// Object file helpers
//

static int sym_index(char *name) {
  intptr_t idx = (intptr_t)hashmap_get(&sym_map, name);
  if (idx)
    return idx - 1;
  if (out->nsyms == out->capsyms) {
    int cap = out->capsyms ? out->capsyms * 2 : 64;
    out->syms = cc_grow(out->syms, sizeof(CcSym) * out->capsyms, sizeof(CcSym) * cap);
    out->capsyms = cap;
  }
  int n = out->nsyms++;
  out->syms[n] = (CcSym){.name = name, .sect = -1, .is_global = 1, .line = cur_line};
  hashmap_put(&sym_map, name, (void *)(intptr_t)(n + 1));
  return n;
}

static void data_reloc(uint32_t offset, int sym, int32_t addend) {
  *cc_new_reloc(out) = (CcReloc){.offset = offset, .addend = addend, .sym = sym, .sect = CC_DATA, .type = CC_R_ABS32};
}

static void add_line(Token *tok) {
  while (tok->origin)
    tok = tok->origin;
  if (!tok->file)
    return;
  char *name = tok->file->name;
  intptr_t f = (intptr_t)hashmap_get(&file_map, name);
  if (!f) {
    if (out->nfiles == out->capfiles) {
      int cap = out->capfiles ? out->capfiles * 2 : 8;
      out->files = cc_grow(out->files, sizeof(char *) * out->capfiles, sizeof(char *) * cap);
      out->capfiles = cap;
    }
    out->files[out->nfiles++] = name;
    f = out->nfiles;
    hashmap_put(&file_map, name, (void *)f);
  }
  cur_line = tok->line_no;

  uint32_t pc = asm_pc();
  while (out->nlines && out->lines[out->nlines - 1].text_off > pc)  // a dropped jump
    out->nlines--;
  if (out->nlines) {
    CcLine *last = &out->lines[out->nlines - 1];
    if (last->file == f - 1 && last->line == tok->line_no)
      return;
    if (last->text_off == pc) {
      last->file = f - 1;
      last->line = tok->line_no;
      return;
    }
  }
  if (out->nlines == out->caplines) {
    int cap = out->caplines ? out->caplines * 2 : 256;
    out->lines = cc_grow(out->lines, sizeof(CcLine) * out->caplines, sizeof(CcLine) * cap);
    out->caplines = cap;
  }
  out->lines[out->nlines++] = (CcLine){pc, f - 1, tok->line_no};
}

static int label_of(char *name) {
  intptr_t l = (intptr_t)hashmap_get(&label_map, name);
  if (!l) {
    l = asm_new_label() + 1;
    hashmap_put(&label_map, name, (void *)l);
  }
  return l - 1;
}

//
// Small instruction helpers
//

static void seqz(int rd, int rs) { rv_opi(3, rd, rs, 1); }           // sltiu rd, rs, 1
static void snez(int rd, int rs) { rv_op(0, 3, rd, ZERO, rs); }      // sltu rd, zero, rs
static void xori(int rd, int rs, int imm) { rv_opi(4, rd, rs, imm); }
static void andi(int rd, int rs, int imm) { rv_opi(7, rd, rs, imm); }
static void slli(int rd, int rs, int sh) { rv_shi(1, 0, rd, rs, sh); }
static void srli(int rd, int rs, int sh) { rv_shi(5, 0, rd, rs, sh); }
static void srai(int rd, int rs, int sh) { rv_shi(5, 0x20, rd, rs, sh); }
static void fmv_x_w(int rd, int fs) { rv_fp(0x70, 0, fs, 0, rd); }
static void fmv_w_x(int fd, int rs) { rv_fp(0x78, 0, rs, 0, fd); }
static void jalr(int rd, int rs) { rv_i(0, rs, 0, rd, OP_JALR); }
static void amo(int f5, int rd, int rs2, int rs1) {
  emit32((uint32_t)f5 << 27 | 3u << 25 | rs2 << 20 | rs1 << 15 | 2 << 12 | rd << 7 | OP_AMO);
}

static void libcall(char *name) { rv_call_sym(sym_index(name), cur_line); }

#if defined(__riscv) && !defined(__riscv_flen)
int cc_soft_float = 1;
#else
int cc_soft_float = 0;
#endif

// is_flt: a float in an FP register (fa0, ft0, fs*). With cc_soft_float a float is a 32-bit
// integer value in a0 instead, and is_sflt holds; its arithmetic calls libgcc.
static bool is_flt(Type *ty) { return ty->kind == TY_FLOAT && !cc_soft_float; }
static bool is_sflt(Type *ty) { return ty->kind == TY_FLOAT && cc_soft_float; }
static bool is_dbl(Type *ty) { return ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE; }
static bool is_i64(Type *ty) { return ty->kind == TY_LONG; }
static bool is_wide(Type *ty) { return is_i64(ty) || is_dbl(ty); }
static bool is_aggr(Type *ty) { return ty->kind == TY_STRUCT || ty->kind == TY_UNION; }
static bool is_byref_type(Type *ty) { return is_aggr(ty) && ty->size > 8; }

// Type an argument is passed as: arrays and functions (variadic arguments do not decay in the
// parser) are pointers.
static Type *arg_type(Type *ty) {
  if (ty->kind == TY_ARRAY || ty->kind == TY_VLA || ty->kind == TY_FUNC)
    return ty_uint;
  return ty;
}

//
// Push / pop
//

static void fmv_s(int fd, int fs) {
  if (fd != fs)
    rv_fp(0x10, fs, fs, 0, fd);  // fsgnj.s fd, fs, fs
}

static int take_x(void) {
  if (nvar_x + tmp_x >= 11)
    return -1;
  int r = sregs[10 - tmp_x++];
  used_x |= 1u << r;
  return r;
}

static int take_f(void) {
  if (nvar_f + tmp_f >= 12)
    return -1;
  int r = fsregs[11 - tmp_f++];
  used_f |= 1u << r;
  return r;
}

static Slot *new_slot(void) {
  if (nslots == MAX_SLOTS)
    error("expression too complex");
  Slot *s = &slots[nslots++];
  *s = (Slot){-1, -1, false, 0};
  return s;
}

static void push(void) {
  Slot *s = new_slot();
  s->reg = take_x();
  if (s->reg >= 0) {
    rv_mv(s->reg, A0);
    return;
  }
  rv_addi(SP, SP, -4);
  rv_store(A0, SP, 0, 4);
  depth += 4;
  s->mem = depth;
}

static void push_val(Type *ty) {
  if (is_flt(ty)) {
    Slot *s = new_slot();
    s->fp = true;
    s->reg = take_f();
    if (s->reg >= 0) {
      fmv_s(s->reg, FA0);
      return;
    }
    rv_addi(SP, SP, -4);
    rv_fsw(FA0, SP, 0);
    depth += 4;
    s->mem = depth;
  } else if (is_wide(ty)) {
    Slot *s = new_slot();
    if (nvar_x + tmp_x + 2 <= 11) {
      s->reg = take_x();
      s->reg2 = take_x();
      rv_mv(s->reg, A0);
      rv_mv(s->reg2, A1);
      return;
    }
    rv_addi(SP, SP, -8);
    rv_store(A0, SP, 0, 4);
    rv_store(A1, SP, 4, 4);
    depth += 8;
    s->mem = depth;
  } else {
    push();
  }
}

// Pops a word; returns the register holding it (scratch if it was in memory). A register
// stays valid until the next push.
static int pop_reg(int scratch) {
  Slot s = slots[--nslots];
  if (s.reg >= 0) {
    tmp_x--;
    return s.reg;
  }
  rv_load(scratch, SP, 0, 4, 0);
  rv_addi(SP, SP, 4);
  depth -= 4;
  return scratch;
}

static void pop(int reg) { rv_mv(reg, pop_reg(reg)); }

// Pops a float; returns the register holding it (scratch if it was in memory).
static int pop_freg(int scratch) {
  Slot s = slots[--nslots];
  if (s.reg >= 0) {
    tmp_f--;
    return s.reg;
  }
  rv_flw(scratch, SP, 0);
  rv_addi(SP, SP, 4);
  depth -= 4;
  return scratch;
}

// Pops a long long / double into lo:hi.
static void pop_wide(int lo, int hi) {
  Slot s = slots[--nslots];
  if (s.reg >= 0) {
    tmp_x -= 2;
    rv_mv(lo, s.reg);
    rv_mv(hi, s.reg2);
    return;
  }
  rv_load(lo, SP, 0, 4, 0);
  rv_load(hi, SP, 4, 4, 0);
  rv_addi(SP, SP, 8);
  depth -= 8;
}

//
// Memory
//

// Copies size bytes from src to dst (registers other than t0, t1, t3, t4).
static void copy_mem(int dst, int src, int size, int align) {
  int w = align >= 4 ? 4 : align >= 2 ? 2 : 1;
  int n = size / w;
  if (n <= 16) {
    for (int i = 0; i < n; i++) {
      rv_load(T1, src, i * w, w, 1);
      rv_store(T1, dst, i * w, w);
    }
  } else {
    rv_mv(T3, src);
    rv_mv(T4, dst);
    rv_li(T0, n);
    int l = asm_new_label();
    asm_bind(l);
    rv_load(T1, T3, 0, w, 1);
    rv_store(T1, T4, 0, w);
    rv_addi(T3, T3, w);
    rv_addi(T4, T4, w);
    rv_addi(T0, T0, -1);
    rv_branch(BNE, T0, ZERO, l);
    rv_addr(T3, T3, -n * w);
    rv_addr(T4, T4, -n * w);
  }
  for (int i = n * w; i < size; i++) {
    rv_load(T1, src, i, 1, 1);
    rv_store(T1, dst, i, 1);
  }
}

// Writes the low n (1..4) bytes of reg to base+off.
static void store_bytes(int reg, int base, int off, int n, int align) {
  if (n == 4 && align >= 4) {
    rv_store(reg, base, off, 4);
    return;
  }
  if (n == 2 && align >= 2) {
    rv_store(reg, base, off, 2);
    return;
  }
  for (int b = 0; b < n; b++) {
    if (b == 0) {
      rv_store(reg, base, off, 1);
    } else {
      srli(T1, reg, 8 * b);
      rv_store(T1, base, off + b, 1);
    }
  }
}

// Loads n (1..4) bytes at base+off into rd (rd may equal base).
static void load_bytes(int rd, int base, int off, int n, int align) {
  if (n == 4 && align >= 4) {
    rv_load(rd, base, off, 4, 0);
    return;
  }
  if (n == 2 && align >= 2) {
    rv_load(rd, base, off, 2, 1);
    return;
  }
  rv_load(T3, base, off, 1, 1);
  for (int b = 1; b < n; b++) {
    rv_load(T4, base, off + b, 1, 1);
    slli(T4, T4, 8 * b);
    rv_op(0, 6, T3, T3, T4);
  }
  rv_mv(rd, T3);
}

// A memory location: base register + offset, or (sym >= 0) a symbol + offset reached with an
// auipc pair.
typedef struct {
  int base;
  int32_t off;
  int sym;
} Addr;

static Addr base_addr(int base, int32_t off) { return (Addr){base, off, -1}; }

// rd = the address
static void addr_to(int rd, Addr a) {
  if (a.sym >= 0)
    rv_sym_addr(rd, a.sym, a.off, cur_line);
  else if (a.off)
    rv_addr(rd, a.base, a.off);
  else
    rv_mv(rd, a.base);
}

// Loads a value of type ty from a into the accumulator.
static void load_from(Type *ty, Addr a) {
  if (is_sflt(ty))
    ty = ty_uint;
  switch (ty->kind) {
  case TY_ARRAY:
  case TY_STRUCT:
  case TY_UNION:
  case TY_FUNC:
  case TY_VLA:
    addr_to(A0, a);
    return;
  case TY_FLOAT:
    if (a.sym >= 0) {
      rv_sym_hi(T5, a.sym, a.off, cur_line);
      rv_flw(FA0, T5, 0);
    } else {
      rv_flw(FA0, a.base, a.off);
    }
    return;
  case TY_DOUBLE:
  case TY_LDOUBLE:
  case TY_LONG:
    if (a.sym >= 0) {
      addr_to(A0, a);
      a = base_addr(A0, 0);
    }
    rv_load(A1, a.base, a.off + 4, 4, 0);
    rv_load(A0, a.base, a.off, 4, 0);
    return;
  default:
    if (a.sym >= 0) {
      rv_sym_hi(A0, a.sym, a.off, cur_line);
      a = base_addr(A0, 0);
    }
    rv_load(A0, a.base, a.off, ty->size, ty->is_unsigned);
  }
}

// Loads a value of type ty from the address in a0.
static void load(Type *ty) { load_from(ty, base_addr(A0, 0)); }

// Stores the accumulator (a struct: the struct at the address in a0) to a.
static void store_to(Type *ty, Addr a) {
  if (is_sflt(ty))
    ty = ty_uint;
  switch (ty->kind) {
  case TY_STRUCT:
  case TY_UNION:
    addr_to(T2, a);
    copy_mem(T2, A0, ty->size, ty->align);
    return;
  case TY_FLOAT:
    if (a.sym >= 0) {
      rv_sym_hi(T2, a.sym, a.off, cur_line);
      a = base_addr(T2, 0);
    }
    rv_fsw(FA0, a.base, a.off);
    return;
  case TY_DOUBLE:
  case TY_LDOUBLE:
  case TY_LONG:
    if (a.sym >= 0) {
      addr_to(T2, a);
      a = base_addr(T2, 0);
    }
    rv_store(A0, a.base, a.off, 4);
    rv_store(A1, a.base, a.off + 4, 4);
    return;
  default:
    if (a.sym >= 0) {
      rv_sym_hi(T2, a.sym, a.off, cur_line);
      a = base_addr(T2, 0);
    }
    rv_store(A0, a.base, a.off, ty->size);
  }
}

// Stores the accumulator to the address on top of the push stack.
static void store(Type *ty) { store_to(ty, base_addr(pop_reg(T2), 0)); }

// Zeroes a local variable (slots are 4-byte aligned and padded to 4 bytes).
static void zero_local(Obj *var) {
  if (var->reg) {
    if (is_flt(var->ty))
      fmv_w_x(var->reg, ZERO);
    else
      rv_mv(var->reg, ZERO);
    return;
  }
  int n = align_to(var->ty->size, 4) / 4;
  if (n <= 16) {
    for (int i = 0; i < n; i++)
      rv_store(ZERO, S0, var->offset + i * 4, 4);
    return;
  }
  rv_addr(T3, S0, var->offset);
  rv_li(T0, n);
  int l = asm_new_label();
  asm_bind(l);
  rv_store(ZERO, T3, 0, 4);
  rv_addi(T3, T3, 4);
  rv_addi(T0, T0, -1);
  rv_branch(BNE, T0, ZERO, l);
}

//
// Addresses
//

static Addr gen_lvalue(Node *node);
static void gen_addr(Node *node);

// Where the pointer expression e points (for an array: where the array is).
static Addr ptr_base(Node *e) {
  if (e->ty->kind == TY_ARRAY)
    return gen_lvalue(e);
  if (e->kind == ND_VAR && e->var->reg)
    return base_addr(e->var->reg, 0);
  gen_expr(e);
  return base_addr(A0, 0);
}

// Evaluates an lvalue to a location, generating as little code as it can: a local in memory,
// a member, a global or `*(p + constant)` cost nothing.
static Addr gen_lvalue(Node *node) {
  switch (node->kind) {
  case ND_VAR: {
    Obj *var = node->var;
    if (var->reg)
      error_tok(node->tok, "internal error: address of a register variable");
    if (var->is_local) {
      if (var->ty->kind == TY_VLA || var->is_byref) {
        rv_load(A0, S0, var->offset, 4, 0);
        return base_addr(A0, 0);
      }
      return base_addr(S0, var->offset);
    }
    if (var->is_tls)
      error_tok(node->tok, "thread-local variables are not supported");
    return (Addr){0, var->offset, sym_index(var->name)};  // offset: see codegen_string()
  }
  case ND_MEMBER: {
    Addr a = gen_lvalue(node->lhs);
    a.off += node->member->offset;
    return a;
  }
  case ND_DEREF: {
    Node *e = node->lhs;
    int64_t k;
    if ((e->kind == ND_ADD || e->kind == ND_SUB) && e->lhs->ty->base && const_int(e->rhs, &k) &&
        k > -(1 << 28) && k < (1 << 28)) {
      Addr a = ptr_base(e->lhs);
      a.off += e->kind == ND_ADD ? k : -k;
      return a;
    }
    return ptr_base(e);
  }
  case ND_COMMA:
    gen_void(node->lhs);
    return gen_lvalue(node->rhs);
  default:
    gen_addr(node);
    return base_addr(A0, 0);
  }
}

// True if gen_lvalue(node) generates no code and gives a location that evaluating other
// expressions does not change.
static bool is_stable_lvalue(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    return !node->var->reg && !(node->var->is_local && (node->var->ty->kind == TY_VLA || node->var->is_byref)) &&
           !(!node->var->is_local && node->var->is_tls);
  case ND_MEMBER:
    return is_stable_lvalue(node->lhs);
  case ND_DEREF: {
    Node *e = node->lhs;
    int64_t k;
    if ((e->kind == ND_ADD || e->kind == ND_SUB) && e->lhs->ty->base && const_int(e->rhs, &k) &&
        k > -(1 << 28) && k < (1 << 28))
      e = e->lhs;
    if (e->ty->kind == TY_ARRAY)
      return is_stable_lvalue(e);
    return e->kind == ND_VAR && e->var->reg;
  }
  default:
    return false;
  }
}

static void gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR:
  case ND_MEMBER:
  case ND_DEREF:
  case ND_COMMA:
    addr_to(A0, gen_lvalue(node));
    return;
  case ND_FUNCALL:
    if (node->ret_buffer) {
      gen_expr(node);
      return;
    }
    break;
  case ND_ASSIGN:
  case ND_COND:
    if (is_aggr(node->ty)) {
      gen_expr(node);
      return;
    }
    break;
  case ND_VLA_PTR:
    rv_addr(A0, S0, node->var->offset);
    return;
  default:
    break;
  }
  error_tok(node->tok, "not an lvalue");
}

//
// Conversions
//

enum { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64 };

static int type_id(Type *ty) {
  switch (ty->kind) {
  case TY_CHAR:
    return ty->is_unsigned ? U8 : I8;
  case TY_SHORT:
    return ty->is_unsigned ? U16 : I16;
  case TY_INT:
    return ty->is_unsigned ? U32 : I32;
  case TY_LONG:
    return ty->is_unsigned ? U64 : I64;
  case TY_FLOAT:
    return F32;
  case TY_DOUBLE:
  case TY_LDOUBLE:
    return F64;
  case TY_BOOL:
    return U8;
  case TY_ENUM:
    return I32;
  default:
    return U32;
  }
}

// Re-extends r to the canonical 32-bit form of an integer type.
static void narrow_reg(int r, int id) {
  switch (id) {
  case I8:
    slli(r, r, 24);
    srai(r, r, 24);
    break;
  case U8:
    andi(r, r, 0xff);
    break;
  case I16:
    slli(r, r, 16);
    srai(r, r, 16);
    break;
  case U16:
    slli(r, r, 16);
    srli(r, r, 16);
    break;
  }
}

static void narrow(int id) { narrow_reg(A0, id); }

// a0 = (accumulator != 0)
static void to_bool(Type *ty) {
  if (is_flt(ty) || is_sflt(ty)) {
    if (is_flt(ty))
      fmv_x_w(T0, FA0);
    slli(T0, is_flt(ty) ? T0 : A0, 1);  // ignore the sign: -0.0 is false
    snez(A0, T0);
  } else if (is_dbl(ty)) {
    slli(T0, A1, 1);
    rv_op(0, 6, T0, T0, A0);
    snez(A0, T0);
  } else if (is_i64(ty)) {
    rv_op(0, 6, A0, A0, A1);
    snez(A0, A0);
  } else {
    snez(A0, A0);
  }
}

// Branches to label if the accumulator (of type ty) is zero (if_zero) or nonzero.
static void branch_zero(Type *ty, int label, bool if_zero) {
  int r = A0;
  if (is_flt(ty) || is_sflt(ty)) {
    if (is_flt(ty))
      fmv_x_w(T0, FA0);
    slli(T0, is_flt(ty) ? T0 : A0, 1);
    r = T0;
  } else if (is_dbl(ty)) {
    slli(T0, A1, 1);
    rv_op(0, 6, T0, T0, A0);
    r = T0;
  } else if (is_i64(ty)) {
    rv_op(0, 6, T0, A0, A1);
    r = T0;
  }
  rv_branch(if_zero ? BEQ : BNE, r, ZERO, label);
}

static void cast(Type *from, Type *to) {
  if (to->kind == TY_VOID)
    return;
  if (to->kind == TY_BOOL) {
    to_bool(from);
    return;
  }
  int t1 = type_id(from), t2 = type_id(to);
  if (t1 == t2)
    return;
  bool f1 = t1 == F32 || t1 == F64, f2 = t2 == F32 || t2 == F64;

  if (!f1 && !f2) {
    if (t2 == I64 || t2 == U64) {
      if (t1 == I64 || t1 == U64)
        return;
      if (t1 == U8 || t1 == U16 || t1 == U32)
        rv_li(A1, 0);
      else
        srai(A1, A0, 31);
      return;
    }
    narrow(t2);
    return;
  }

  if (t1 == F32 && t2 == F64) {
    libcall("__extendsfdf2");
    return;
  }
  if (t1 == F64 && t2 == F32) {
    libcall("__truncdfsf2");
    return;
  }

  if (t1 == F32) {  // float -> integer
    if (t2 == I64)
      libcall("__fixsfdi");
    else if (t2 == U64)
      libcall("__fixunssfdi");
    else if (t2 == U32 && cc_soft_float)
      libcall("__fixunssfsi");
    else if (t2 == U32)
      rv_fp(0x60, 1, FA0, 1, A0);  // fcvt.wu.s a0, fa0, rtz
    else {
      if (cc_soft_float)
        libcall("__fixsfsi");
      else
        rv_fp(0x60, 0, FA0, 1, A0);  // fcvt.w.s a0, fa0, rtz
      narrow(t2);
    }
    return;
  }
  if (t1 == F64) {  // double -> integer
    if (t2 == I64)
      libcall("__fixdfdi");
    else if (t2 == U64)
      libcall("__fixunsdfdi");
    else if (t2 == U32)
      libcall("__fixunsdfsi");
    else {
      libcall("__fixdfsi");
      narrow(t2);
    }
    return;
  }

  // integer -> floating point
  if (t1 == I64 || t1 == U64) {
    if (t2 == F32)
      libcall(t1 == I64 ? "__floatdisf" : "__floatundisf");
    else
      libcall(t1 == I64 ? "__floatdidf" : "__floatundidf");
    return;
  }
  bool u = t1 == U32;  // smaller unsigned types are zero-extended, so a signed conversion is exact
  if (t2 == F32 && cc_soft_float)
    libcall(u ? "__floatunsisf" : "__floatsisf");
  else if (t2 == F32)
    rv_fp(0x68, u ? 1 : 0, A0, 7, FA0);  // fcvt.s.w[u] fa0, a0
  else
    libcall(u ? "__floatunsidf" : "__floatsidf");
}

//
// Constants and leaves
//

static int64_t normalize(int64_t v, Type *ty) {
  if (ty->kind == TY_BOOL)
    return v != 0;
  switch (ty->size) {
  case 1: return ty->is_unsigned ? (int64_t)(uint8_t)v : (int64_t)(int8_t)v;
  case 2: return ty->is_unsigned ? (int64_t)(uint16_t)v : (int64_t)(int16_t)v;
  case 4: return ty->is_unsigned ? (int64_t)(uint32_t)v : (int64_t)(int32_t)v;
  default: return v;
  }
}

// If node is an integer constant expression, stores its value in *v.
static bool const_int(Node *node, int64_t *v) {
  Type *ty = node->ty;
  if (!ty || !(is_integer(ty) || ty->kind == TY_PTR))
    return false;
  int64_t a, b;
  switch (node->kind) {
  case ND_NUM:
    a = node->val;
    break;
  case ND_CAST:
    if (!const_int(node->lhs, &a))
      return false;
    break;
  case ND_NEG:
  case ND_BITNOT:
    if (!const_int(node->lhs, &a))
      return false;
    a = node->kind == ND_NEG ? (int64_t)(0 - (uint64_t)a) : ~a;
    break;
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_SHL:
  case ND_SHR: {
    if (!const_int(node->lhs, &a) || !const_int(node->rhs, &b))
      return false;
    Type *lt = node->lhs->ty;
    bool u = lt->is_unsigned;
    bool w = lt->size == 8;
    switch (node->kind) {
    case ND_ADD: a = (int64_t)((uint64_t)a + (uint64_t)b); break;
    case ND_SUB: a = (int64_t)((uint64_t)a - (uint64_t)b); break;
    case ND_MUL: a = (int64_t)((uint64_t)a * (uint64_t)b); break;
    case ND_DIV:
    case ND_MOD:
      if (b == 0 || (!u && b == -1))
        return false;
      if (u && !w)
        a = node->kind == ND_DIV ? (uint32_t)a / (uint32_t)b : (uint32_t)a % (uint32_t)b;
      else if (u)
        a = (int64_t)(node->kind == ND_DIV ? (uint64_t)a / (uint64_t)b : (uint64_t)a % (uint64_t)b);
      else
        a = node->kind == ND_DIV ? a / b : a % b;
      break;
    case ND_BITAND: a &= b; break;
    case ND_BITOR: a |= b; break;
    case ND_BITXOR: a ^= b; break;
    case ND_SHL: a = (int64_t)((uint64_t)a << (b & (w ? 63 : 31))); break;
    case ND_SHR:
      if (u)
        a = w ? (int64_t)((uint64_t)a >> (b & 63)) : (int64_t)((uint32_t)a >> (b & 31));
      else
        a = w ? a >> (b & 63) : (int64_t)((int32_t)a >> (b & 31));
      break;
    default: break;
    }
    break;
  }
  default:
    return false;
  }
  *v = normalize(a, ty);
  return true;
}

// Scalars of at most 32 bits: what a register variable or a leaf can be.
static bool is_scalar32(Type *ty) {
  switch (ty->kind) {
  case TY_BOOL:
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_ENUM:
  case TY_PTR:
  case TY_FLOAT:
    return true;
  default:
    return false;
  }
}

// Integer, pointer or float of at most 32 bits whose value is in a0 (not fa0).
static bool is_int32(Type *ty) { return is_scalar32(ty) && ty->kind != TY_FLOAT; }

// A cast that generates no code.
static bool is_noop_cast(Type *from, Type *to) {
  if (to->kind == TY_BOOL || to->kind == TY_VOID)
    return false;
  int t1 = type_id(from), t2 = type_id(to);
  if (t1 == t2)
    return true;
  return (t2 == I32 || t2 == U32) && t1 != I64 && t1 != U64 && t1 != F32 && t1 != F64;
}

// The address of a variable, if it needs no code beyond one auipc pair or addi.
static bool is_simple_var(Obj *var) {
  if (var->is_local)
    return var->ty->kind != TY_VLA && !var->is_byref;
  return !var->is_tls;
}

// Leaves: values that can be computed straight into any register, with no temporaries
// (only t5, and t0 for a float constant) and no side effects.
static bool is_leaf(Node *node) {
  switch (node->kind) {
  case ND_NUM:
    return is_scalar32(node->ty);
  case ND_VAR: {
    Obj *var = node->var;
    if (var->reg)
      return true;
    if (!is_simple_var(var))
      return false;
    return node->ty->kind == TY_ARRAY || node->ty->kind == TY_FUNC || is_scalar32(node->ty);
  }
  case ND_ADDR:
    return node->lhs->kind == ND_VAR && !node->lhs->var->reg && is_simple_var(node->lhs->var);
  case ND_CAST:
    return is_noop_cast(node->lhs->ty, node->ty) && is_leaf(node->lhs);
  default:
    return false;
  }
}

static void gen_leaf(Node *node, int rd) {
  switch (node->kind) {
  case ND_NUM:
    if (is_sflt(node->ty)) {
      float f = node->fval;
      uint32_t u;
      memcpy(&u, &f, 4);
      rv_li(rd, (int32_t)u);
    } else if (is_flt(node->ty)) {
      float f = node->fval;
      uint32_t u;
      memcpy(&u, &f, 4);
      if (u) {
        rv_li(T0, u);
        fmv_w_x(rd, T0);
      } else {
        fmv_w_x(rd, ZERO);
      }
    } else {
      rv_li(rd, (int32_t)(uint32_t)node->val);
    }
    return;
  case ND_VAR: {
    Obj *var = node->var;
    if (var->reg) {
      if (is_flt(var->ty))
        fmv_s(rd, var->reg);
      else
        rv_mv(rd, var->reg);
      return;
    }
    Type *ty = node->ty;
    if (ty->kind == TY_ARRAY || ty->kind == TY_FUNC) {
      addr_to(rd, gen_lvalue(node));
      return;
    }
    Addr a = gen_lvalue(node);
    if (is_flt(ty)) {
      if (a.sym >= 0) {
        rv_sym_hi(T5, a.sym, a.off, cur_line);
        a = base_addr(T5, 0);
      }
      rv_flw(rd, a.base, a.off);
    } else {
      if (a.sym >= 0) {
        rv_sym_hi(rd, a.sym, a.off, cur_line);
        a = base_addr(rd, 0);
      }
      rv_load(rd, a.base, a.off, ty->size, ty->is_unsigned);
    }
    return;
  }
  case ND_ADDR:
    addr_to(rd, gen_lvalue(node->lhs));
    return;
  case ND_CAST:
    gen_leaf(node->lhs, rd);
    return;
  default:
    error_tok(node->tok, "internal error: not a leaf");
  }
}

// The register holding a leaf's value: its own register, zero, or scratch.
static int operand(Node *node, int scratch) {
  while (node->kind == ND_CAST)
    node = node->lhs;
  if (node->kind == ND_VAR && node->var->reg)
    return node->var->reg;
  if (node->kind == ND_NUM && node->ty->kind != TY_FLOAT && !node->val)
    return ZERO;
  gen_leaf(node, scratch);
  return scratch;
}

// Evaluates node into the accumulator (or leaves it where it already is): the register
// holding its value.
static int gen_value(Node *node, int acc) {
  if (is_leaf(node))
    return operand(node, acc);
  gen_expr(node);
  return acc;
}

//
// Calling convention
//

enum { L_GPR, L_GPR2, L_FPR, L_STACK, L_SPLIT };
typedef struct { int kind, reg, off; } Loc;
typedef struct { int gp, fp, stack; } AbiState;

static Loc classify(AbiState *s, Type *ty, bool variadic) {
  Loc l = {0};
  if (is_byref_type(ty))
    ty = ty_uint;  // a pointer to the copy
  if (is_flt(ty) && !variadic && s->fp < 8) {
    l.kind = L_FPR;
    l.reg = s->fp++;
    return l;
  }
  if (ty->size <= 4) {
    if (s->gp < 8) {
      l.kind = L_GPR;
      l.reg = s->gp++;
      return l;
    }
    l.kind = L_STACK;
    l.off = s->stack;
    s->stack += 4;
    return l;
  }
  // 5..8 bytes: register pair, a7 + stack word, or stack
  if (variadic && ty->align == 8 && (s->gp & 1))
    s->gp++;
  if (s->gp <= 6) {
    l.kind = L_GPR2;
    l.reg = s->gp;
    s->gp += 2;
    return l;
  }
  if (s->gp == 7) {
    l.kind = L_SPLIT;
    l.reg = 7;
    s->gp = 8;
    l.off = s->stack;
    s->stack += 4;
    return l;
  }
  s->gp = 8;
  s->stack = align_to(s->stack, ty->align > 4 ? 8 : 4);
  l.kind = L_STACK;
  l.off = s->stack;
  s->stack += align_to(ty->size, 4);
  return l;
}

static void builtin_alloca(void) {
  rv_addi(A0, A0, 15);
  andi(A0, A0, -16);
  // Move the temporary area [sp, alloca_bottom) down by a0 bytes.
  rv_load(T0, S0, current_fn->alloca_bottom->offset, 4, 0);
  rv_sub(T1, T0, SP);
  rv_mv(T3, SP);
  rv_sub(SP, SP, A0);
  rv_mv(T4, SP);
  int loop = asm_new_label(), done = asm_new_label();
  asm_bind(loop);
  rv_branch(BEQ, T1, ZERO, done);
  rv_load(T2, T3, 0, 4, 0);
  rv_store(T2, T4, 0, 4);
  rv_addi(T3, T3, 4);
  rv_addi(T4, T4, 4);
  rv_addi(T1, T1, -4);
  rv_jump(loop);
  asm_bind(done);
  rv_sub(A0, T0, A0);
  rv_store(A0, S0, current_fn->alloca_bottom->offset, 4);
}

static void gen_funcall(Node *node) {
  if (node->lhs->kind == ND_VAR && !strcmp(node->lhs->var->name, "alloca")) {
    gen_expr(node->args);
    builtin_alloca();
    return;
  }

  Type *fty = node->func_ty;
  int nparams = 0;
  while (fty->params && fty->params[nparams])
    nparams++;
  int nargs = 0;
  for (Node *a = node->args; a; a = a->next)
    nargs++;

  Loc *locs = cc_fn_calloc((nargs + 1) * sizeof(Loc));
  int *slot = cc_fn_calloc((nargs + 1) * sizeof(int));  // push slot, -1: a leaf, -2: an aggregate in memory
  int *mem = cc_fn_calloc((nargs + 1) * sizeof(int));   // depth after copying an aggregate
  AbiState st = {0};
  bool sret = node->ret_buffer && node->ty->size > 8;
  if (sret)
    st.gp = 1;
  int i = 0;
  for (Node *a = node->args; a; a = a->next, i++)
    locs[i] = classify(&st, arg_type(a->ty), i >= nparams && fty->is_variadic);

  int base_depth = depth, base_slots = nslots, base_x = tmp_x, base_f = tmp_f;

  // Evaluate the arguments left to right onto the push stack; leaves are loaded last.
  i = 0;
  for (Node *a = node->args; a; a = a->next, i++) {
    Type *ty = arg_type(a->ty);
    if (is_leaf(a) && !is_aggr(ty)) {
      slot[i] = -1;
      continue;
    }
    gen_expr(a);
    if (is_byref_type(ty)) {
      int sz = align_to(ty->size, 8);
      sz += (8 - (depth + sz) % 8) % 8;
      rv_addr(SP, SP, -sz);
      depth += sz;
      copy_mem(SP, A0, ty->size, ty->align);
      rv_mv(A0, SP);
      push();
      slot[i] = nslots - 1;
    } else if (is_aggr(ty)) {
      int sz = align_to(ty->size, 4);
      rv_addi(SP, SP, -sz);
      depth += sz;
      copy_mem(SP, A0, ty->size, ty->align);
      slot[i] = -2;
      mem[i] = depth;
    } else {
      push_val(ty);
      slot[i] = nslots - 1;
    }
  }

  bool direct = node->lhs->kind == ND_VAR && node->lhs->var->ty->kind == TY_FUNC &&
                !node->lhs->var->is_local;
  bool fn_leaf = !direct && is_leaf(node->lhs);
  int fn_slot = -1;
  if (!direct && !fn_leaf) {
    gen_expr(node->lhs);
    push();
    fn_slot = nslots - 1;
  }

  // Outgoing stack arguments at the new sp, which is 16-byte aligned.
  int stack_bytes = align_to(st.stack, 4);
  int area = stack_bytes + (16 - (depth + stack_bytes) % 16) % 16;
  if (area) {
    rv_addr(SP, SP, -area);
    depth += area;
  }

  // Pushed arguments
  i = 0;
  for (Node *a = node->args; a; a = a->next, i++) {
    if (slot[i] == -1)
      continue;
    Loc l = locs[i];
    Type *ty = arg_type(a->ty);
    if (slot[i] == -2 || slots[slot[i]].reg < 0) {
      int src = depth - (slot[i] == -2 ? mem[i] : slots[slot[i]].mem);
      int words = is_byref_type(ty) ? 1 : align_to(ty->size, 4) / 4;
      switch (l.kind) {
      case L_STACK:
        for (int w = 0; w < words; w++) {
          rv_load(T0, SP, src + 4 * w, 4, 0);
          rv_store(T0, SP, l.off + 4 * w, 4);
        }
        break;
      case L_SPLIT:
        rv_load(T0, SP, src + 4, 4, 0);
        rv_store(T0, SP, l.off, 4);
        rv_load(A7, SP, src, 4, 0);
        break;
      case L_GPR:
        rv_load(A0 + l.reg, SP, src, 4, 0);
        break;
      case L_GPR2:
        rv_load(A0 + l.reg, SP, src, 4, 0);
        rv_load(A0 + l.reg + 1, SP, src + 4, 4, 0);
        break;
      case L_FPR:
        rv_flw(FA0 + l.reg, SP, src);
        break;
      }
      continue;
    }
    Slot *s = &slots[slot[i]];
    switch (l.kind) {
    case L_STACK:
      if (s->fp) {
        rv_fsw(s->reg, SP, l.off);
      } else {
        rv_store(s->reg, SP, l.off, 4);
        if (s->reg2 >= 0)
          rv_store(s->reg2, SP, l.off + 4, 4);
      }
      break;
    case L_SPLIT:
      rv_store(s->reg2, SP, l.off, 4);
      rv_mv(A7, s->reg);
      break;
    case L_GPR:
      if (s->fp)
        fmv_x_w(A0 + l.reg, s->reg);
      else
        rv_mv(A0 + l.reg, s->reg);
      break;
    case L_GPR2:
      rv_mv(A0 + l.reg, s->reg);
      rv_mv(A0 + l.reg + 1, s->reg2);
      break;
    case L_FPR:
      fmv_s(FA0 + l.reg, s->reg);
      break;
    }
  }

  // Leaves
  i = 0;
  for (Node *a = node->args; a; a = a->next, i++) {
    if (slot[i] != -1)
      continue;
    Loc l = locs[i];
    bool f = is_flt(a->ty);
    switch (l.kind) {
    case L_STACK:
      if (f) {
        gen_leaf(a, FT0);
        rv_fsw(FT0, SP, l.off);
      } else {
        rv_store(operand(a, T0), SP, l.off, 4);
      }
      break;
    case L_GPR:
      if (f) {
        gen_leaf(a, FT0);
        fmv_x_w(A0 + l.reg, FT0);
      } else {
        gen_leaf(a, A0 + l.reg);
      }
      break;
    case L_FPR:
      gen_leaf(a, FA0 + l.reg);
      break;
    default:
      error_tok(a->tok, "internal error: leaf argument");
    }
  }

  if (sret)
    rv_addr(A0, S0, node->ret_buffer->offset);
  if (direct) {
    rv_call_sym(sym_index(node->lhs->var->name), cur_line);
  } else if (fn_leaf) {
    gen_leaf(node->lhs, T6);
    jalr(RA, T6);
  } else if (slots[fn_slot].reg >= 0) {
    jalr(RA, slots[fn_slot].reg);
  } else {
    rv_load(T6, SP, depth - slots[fn_slot].mem, 4, 0);
    jalr(RA, T6);
  }

  if (depth != base_depth)
    rv_addr(SP, SP, depth - base_depth);
  depth = base_depth;
  nslots = base_slots;
  tmp_x = base_x;
  tmp_f = base_f;

  Type *rt = node->ty;
  if (is_aggr(rt)) {
    Obj *rb = node->ret_buffer;
    if (rt->size <= 8) {
      rv_addr(T2, S0, rb->offset);
      store_bytes(A0, T2, 0, rt->size < 4 ? rt->size : 4, rt->align);
      if (rt->size > 4)
        store_bytes(A1, T2, 4, rt->size - 4, rt->align);
      rv_mv(A0, T2);
    } else {
      rv_addr(A0, S0, rb->offset);
    }
    return;
  }
  if (rt->kind == TY_CHAR || rt->kind == TY_SHORT)
    narrow(type_id(rt));
  else if (rt->kind == TY_BOOL)
    andi(A0, A0, 0xff);
}

//
// Expressions
//

static bool fits12(int64_t v) { return v >= -2048 && v <= 2047; }

static int log2_exact(uint32_t v) {
  if (!v || (v & (v - 1)))
    return -1;
  int n = 0;
  while (v >>= 1)
    n++;
  return n;
}

// Evaluates two 32-bit integer operands; *a and *b are the registers holding them (a0 or a
// variable's register for lhs, t0 or a register for rhs).
static void gen_operands(Node *lhs, Node *rhs, int *a, int *b) {
  if (is_leaf(rhs)) {
    *a = gen_value(lhs, A0);
    *b = operand(rhs, T0);
  } else if (is_leaf(lhs)) {
    gen_expr(rhs);
    *b = A0;
    *a = operand(lhs, T0);
  } else {
    gen_expr(rhs);
    push();
    gen_expr(lhs);
    *b = pop_reg(T0);
    *a = A0;
  }
}

// The same for floats (fa0 / ft0).
static void gen_foperands(Node *lhs, Node *rhs, int *a, int *b) {
  if (is_leaf(rhs)) {
    *a = gen_value(lhs, FA0);
    *b = operand(rhs, FT0);
  } else if (is_leaf(lhs)) {
    gen_expr(rhs);
    *b = FA0;
    *a = operand(lhs, FT0);
  } else {
    gen_expr(rhs);
    push_val(rhs->ty);
    gen_expr(lhs);
    *b = pop_freg(FT0);
    *a = FA0;
  }
}

static void lt64(int alo, int ahi, int blo, int bhi, bool is_unsigned) {
  rv_op(0, is_unsigned ? 3 : 2, T2, ahi, bhi);  // hi <
  rv_op(0, 4, T3, ahi, bhi);
  seqz(T3, T3);                                 // hi ==
  rv_op(0, 3, T4, alo, blo);                    // lo <u
  rv_op(0, 7, T3, T3, T4);
  rv_op(0, 6, A0, T2, T3);
}

// 32-bit integer operation with a constant rhs; false if there is no short form.
static bool gen_binary_imm(Node *node, int64_t k) {
  Node *lhs = node->lhs;
  bool u = lhs->ty->is_unsigned;
  int32_t imm = (int32_t)(uint32_t)k;
  int sh = log2_exact((uint32_t)imm);
  switch (node->kind) {
  case ND_ADD:
    if (!fits12(imm))
      return false;
    rv_addi(A0, gen_value(lhs, A0), imm);
    return true;
  case ND_SUB:
    if (!fits12(-(int64_t)imm))
      return false;
    rv_addi(A0, gen_value(lhs, A0), -imm);
    return true;
  case ND_MUL:
    if (sh < 0)
      return false;
    slli(A0, gen_value(lhs, A0), sh);
    return true;
  case ND_DIV:
    if (!u || sh < 0)
      return false;
    srli(A0, gen_value(lhs, A0), sh);
    return true;
  case ND_MOD:
    if (!u || sh < 0 || !fits12(imm - 1))
      return false;
    andi(A0, gen_value(lhs, A0), imm - 1);
    return true;
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
    if (!fits12(imm))
      return false;
    rv_opi(node->kind == ND_BITAND ? 7 : node->kind == ND_BITOR ? 6 : 4, A0, gen_value(lhs, A0), imm);
    return true;
  case ND_SHL:
    slli(A0, gen_value(lhs, A0), imm & 31);
    return true;
  case ND_SHR:
    if (u)
      srli(A0, gen_value(lhs, A0), imm & 31);
    else
      srai(A0, gen_value(lhs, A0), imm & 31);
    return true;
  case ND_EQ:
  case ND_NE: {
    if (imm && !fits12(-(int64_t)imm))
      return false;
    int a = gen_value(lhs, A0);
    if (imm) {
      rv_addi(A0, a, -imm);
      a = A0;
    }
    if (node->kind == ND_EQ)
      seqz(A0, a);
    else
      snez(A0, a);
    return true;
  }
  case ND_LT:
    if (!fits12(imm))
      return false;
    rv_opi(u ? 3 : 2, A0, gen_value(lhs, A0), imm);  // slti[u]
    return true;
  case ND_LE:
    if (u ? (uint32_t)imm == 0xffffffffu : imm == INT32_MAX)
      return false;
    if (!fits12((int64_t)imm + 1))
      return false;
    rv_opi(u ? 3 : 2, A0, gen_value(lhs, A0), imm + 1);
    return true;
  default:
    return false;
  }
}

static void gen_binary(Node *node) {
  Type *ty = node->lhs->ty;
  Node *lhs = node->lhs, *rhs = node->rhs;
  int a, b;

  if (node->kind == ND_SHL || node->kind == ND_SHR) {
    if (rhs->ty->kind == TY_FLOAT || is_dbl(rhs->ty))
      error_tok(node->tok, "invalid operand");
    if (is_i64(ty)) {
      // only the low word of the count matters
      gen_expr(rhs);
      push();
      gen_expr(lhs);
      pop(A2);
      libcall(node->kind == ND_SHL ? "__ashldi3" : ty->is_unsigned ? "__lshrdi3" : "__ashrdi3");
      return;
    }
    int64_t k;
    if (const_int(rhs, &k) && gen_binary_imm(node, k))
      return;
    if (is_i64(rhs->ty)) {
      gen_expr(rhs);
      push();
      gen_expr(lhs);
      a = A0;
      b = pop_reg(T0);
    } else {
      gen_operands(lhs, rhs, &a, &b);
    }
    if (node->kind == ND_SHL)
      rv_op(0, 1, A0, a, b);
    else
      rv_op(ty->is_unsigned ? 0 : 0x20, 5, A0, a, b);
    return;
  }

  if (is_flt(ty)) {
    gen_foperands(lhs, rhs, &a, &b);
    switch (node->kind) {
    case ND_ADD: rv_fp(0x00, b, a, 7, FA0); return;
    case ND_SUB: rv_fp(0x04, b, a, 7, FA0); return;
    case ND_MUL: rv_fp(0x08, b, a, 7, FA0); return;
    case ND_DIV: rv_fp(0x0c, b, a, 7, FA0); return;
    case ND_EQ: rv_fp(0x50, b, a, 2, A0); return;
    case ND_NE: rv_fp(0x50, b, a, 2, A0); xori(A0, A0, 1); return;
    case ND_LT: rv_fp(0x50, b, a, 1, A0); return;
    case ND_LE: rv_fp(0x50, b, a, 0, A0); return;
    default: error_tok(node->tok, "invalid expression");
    }
  }

  if (is_sflt(ty)) {
    gen_expr(rhs);
    push();
    gen_expr(lhs);
    pop(A1);
    switch (node->kind) {
    case ND_ADD: libcall("__addsf3"); return;
    case ND_SUB: libcall("__subsf3"); return;
    case ND_MUL: libcall("__mulsf3"); return;
    case ND_DIV: libcall("__divsf3"); return;
    case ND_EQ: libcall("__eqsf2"); seqz(A0, A0); return;
    case ND_NE: libcall("__nesf2"); snez(A0, A0); return;
    case ND_LT: libcall("__ltsf2"); rv_opi(2, A0, A0, 0); return;  // slti a0, a0, 0
    case ND_LE: libcall("__lesf2"); rv_opi(2, A0, A0, 1); return;  // slti a0, a0, 1
    default: error_tok(node->tok, "invalid expression");
    }
  }

  if (is_dbl(ty)) {
    gen_expr(rhs);
    push_val(rhs->ty);
    gen_expr(lhs);
    pop_wide(A2, A3);
    switch (node->kind) {
    case ND_ADD: libcall("__adddf3"); return;
    case ND_SUB: libcall("__subdf3"); return;
    case ND_MUL: libcall("__muldf3"); return;
    case ND_DIV: libcall("__divdf3"); return;
    case ND_EQ: libcall("__eqdf2"); seqz(A0, A0); return;
    case ND_NE: libcall("__nedf2"); snez(A0, A0); return;
    case ND_LT: libcall("__ltdf2"); rv_opi(2, A0, A0, 0); return;  // slti a0, a0, 0
    case ND_LE: libcall("__ledf2"); rv_opi(2, A0, A0, 1); return;  // slti a0, a0, 1
    default: error_tok(node->tok, "invalid expression");
    }
  }

  if (is_i64(ty)) {
    bool u = ty->is_unsigned;
    gen_expr(rhs);
    push_val(rhs->ty);
    gen_expr(lhs);
    if (node->kind == ND_DIV || node->kind == ND_MOD) {
      pop_wide(A2, A3);
      if (node->kind == ND_DIV)
        libcall(u ? "__udivdi3" : "__divdi3");
      else
        libcall(u ? "__umoddi3" : "__moddi3");
      return;
    }
    pop_wide(T0, T1);
    switch (node->kind) {
    case ND_ADD:
      rv_add(A0, A0, T0);
      rv_op(0, 3, T2, A0, T0);
      rv_add(A1, A1, T1);
      rv_add(A1, A1, T2);
      return;
    case ND_SUB:
      rv_op(0, 3, T2, A0, T0);
      rv_sub(A0, A0, T0);
      rv_sub(A1, A1, T1);
      rv_sub(A1, A1, T2);
      return;
    case ND_MUL:
      rv_op(1, 0, T2, A0, T1);  // lo(a) * hi(b)
      rv_op(1, 0, T3, A1, T0);  // hi(a) * lo(b)
      rv_op(1, 3, T4, A0, T0);  // mulhu
      rv_op(1, 0, A0, A0, T0);
      rv_add(A1, T2, T3);
      rv_add(A1, A1, T4);
      return;
    case ND_BITAND:
      rv_op(0, 7, A0, A0, T0);
      rv_op(0, 7, A1, A1, T1);
      return;
    case ND_BITOR:
      rv_op(0, 6, A0, A0, T0);
      rv_op(0, 6, A1, A1, T1);
      return;
    case ND_BITXOR:
      rv_op(0, 4, A0, A0, T0);
      rv_op(0, 4, A1, A1, T1);
      return;
    case ND_EQ:
    case ND_NE:
      rv_op(0, 4, T2, A0, T0);
      rv_op(0, 4, T3, A1, T1);
      rv_op(0, 6, A0, T2, T3);
      if (node->kind == ND_EQ)
        seqz(A0, A0);
      else
        snez(A0, A0);
      return;
    case ND_LT:
      lt64(A0, A1, T0, T1, u);
      return;
    case ND_LE:
      lt64(T0, T1, A0, A1, u);
      xori(A0, A0, 1);
      return;
    default:
      error_tok(node->tok, "invalid expression");
    }
  }

  // 32-bit integers and pointers. A constant goes on the right of a commutative operator.
  int64_t k;
  switch (node->kind) {
  case ND_ADD:
  case ND_MUL:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_EQ:
  case ND_NE:
    if (const_int(lhs, &k) && !const_int(rhs, &k) && lhs->ty->size == rhs->ty->size) {
      Node swapped = *node;
      swapped.lhs = rhs;
      swapped.rhs = lhs;
      if (const_int(lhs, &k) && gen_binary_imm(&swapped, k))
        return;
    }
    break;
  default:
    break;
  }
  if (const_int(rhs, &k) && gen_binary_imm(node, k))
    return;

  bool u = ty->is_unsigned;
  gen_operands(lhs, rhs, &a, &b);
  switch (node->kind) {
  case ND_ADD: rv_add(A0, a, b); return;
  case ND_SUB: rv_sub(A0, a, b); return;
  case ND_MUL: rv_op(1, 0, A0, a, b); return;
  case ND_DIV: rv_op(1, u ? 5 : 4, A0, a, b); return;
  case ND_MOD: rv_op(1, u ? 7 : 6, A0, a, b); return;
  case ND_BITAND: rv_op(0, 7, A0, a, b); return;
  case ND_BITOR: rv_op(0, 6, A0, a, b); return;
  case ND_BITXOR: rv_op(0, 4, A0, a, b); return;
  case ND_EQ: rv_op(0, 4, A0, a, b); seqz(A0, A0); return;
  case ND_NE: rv_op(0, 4, A0, a, b); snez(A0, A0); return;
  case ND_LT: rv_op(0, u ? 3 : 2, A0, a, b); return;
  case ND_LE: rv_op(0, u ? 3 : 2, A0, b, a); xori(A0, A0, 1); return;
  default: error_tok(node->tok, "invalid expression");
  }
}

// Branches to label if the value of node is nonzero (if_true) or zero.
static void gen_cond(Node *node, int label, bool if_true) {
  int64_t k;
  switch (node->kind) {
  case ND_NOT:
    gen_cond(node->lhs, label, !if_true);
    return;
  case ND_LOGAND:
  case ND_LOGOR: {
    bool is_and = node->kind == ND_LOGAND;
    if (is_and == if_true) {
      // and: both must hold to jump; or: both must fail to fall through
      int skip = asm_new_label();
      gen_cond(node->lhs, skip, !if_true);
      gen_cond(node->rhs, label, if_true);
      asm_bind(skip);
    } else {
      gen_cond(node->lhs, label, if_true);
      gen_cond(node->rhs, label, if_true);
    }
    return;
  }
  case ND_CAST:
    if (node->ty->kind == TY_BOOL || is_noop_cast(node->lhs->ty, node->ty)) {
      gen_cond(node->lhs, label, if_true);
      return;
    }
    break;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE: {
    Type *ty = node->lhs->ty;
    if (!is_int32(ty) || !is_int32(node->rhs->ty))
      break;
    bool u = ty->is_unsigned;
    int a, b;
    if (const_int(node->rhs, &k) && !k) {
      a = gen_value(node->lhs, A0);
      b = ZERO;
    } else {
      gen_operands(node->lhs, node->rhs, &a, &b);
    }
    switch (node->kind) {
    case ND_EQ: rv_branch(if_true ? BEQ : BNE, a, b, label); return;
    case ND_NE: rv_branch(if_true ? BNE : BEQ, a, b, label); return;
    case ND_LT: rv_branch(if_true ? (u ? BLTU : BLT) : (u ? BGEU : BGE), a, b, label); return;
    default: rv_branch(if_true ? (u ? BGEU : BGE) : (u ? BLTU : BLT), b, a, label); return;
    }
  }
  default:
    break;
  }
  if (const_int(node, &k)) {
    if ((k != 0) == if_true)
      rv_jump(label);
    return;
  }
  if (is_int32(node->ty) && is_leaf(node)) {
    rv_branch(if_true ? BNE : BEQ, operand(node, A0), ZERO, label);
    return;
  }
  gen_expr(node);
  branch_zero(node->ty, label, !if_true);
}

// Makes the last instruction, which computed the accumulator, write reg instead.
static bool retarget(int reg, bool fp) {
  uint32_t *p = asm_last_insn();
  if (!p)
    return false;
  uint32_t insn = *p, op = insn & 0x7f, rd = insn >> 7 & 31;
  if (fp) {
    if (rd != FA0)
      return false;
    if (op != OP_LOADFP) {
      if (op != OP_FP)
        return false;
      uint32_t f7 = insn >> 25;
      if (f7 != 0x00 && f7 != 0x04 && f7 != 0x08 && f7 != 0x0c && f7 != 0x10 && f7 != 0x14 &&
          f7 != 0x2c && f7 != 0x68 && f7 != 0x78)
        return false;
    }
  } else {
    if (rd != A0 || (op != OP_OP && op != OP_IMM && op != OP_LOAD && op != OP_LUI))
      return false;
  }
  *p = (insn & ~(31u << 7)) | (uint32_t)reg << 7;
  return true;
}

// Stores the 32-bit integer in rs to a.
static void store_reg(int rs, Type *ty, Addr a) {
  if (a.sym >= 0) {
    rv_sym_hi(T2, a.sym, a.off, cur_line);
    a = base_addr(T2, 0);
  }
  rv_store(rs, a.base, a.off, ty->size);
}

static void gen_assign(Node *node, bool is_void) {
  Node *lhs = node->lhs, *rhs = node->rhs;
  Type *ty = node->ty;

  if (lhs->kind == ND_VAR && lhs->var->reg) {
    int r = lhs->var->reg;
    if (is_flt(ty)) {
      if (is_leaf(rhs)) {
        gen_leaf(rhs, r);
        if (!is_void)
          fmv_s(FA0, r);
        return;
      }
      gen_expr(rhs);
      if (!is_void || !retarget(r, true))
        fmv_s(r, FA0);
      return;
    }
    if (is_leaf(rhs)) {
      gen_leaf(rhs, r);
      if (!is_void)
        rv_mv(A0, r);
      return;
    }
    gen_expr(rhs);
    if (!is_void || !retarget(r, false))
      rv_mv(r, A0);
    return;
  }

  if (lhs->kind == ND_MEMBER && lhs->member->is_bitfield) {
    Member *mem = lhs->member;
    if (mem->ty->size > 4)
      error_tok(node->tok, "long long bit-fields are not supported");
    gen_addr(lhs);
    push();
    gen_expr(rhs);
    uint32_t mask = mem->bit_width == 32 ? 0xffffffffu : (1u << mem->bit_width) - 1;
    rv_mv(T6, A0);  // the new value is the result
    rv_li(T5, (int32_t)mask);
    rv_op(0, 7, T4, A0, T5);
    slli(T4, T4, mem->bit_offset);
    Slot *s = &slots[nslots - 1];
    if (s->reg >= 0)
      rv_mv(A0, s->reg);
    else
      rv_load(A0, SP, 0, 4, 0);
    load(mem->ty);
    rv_li(T5, (int32_t)~(mask << mem->bit_offset));
    rv_op(0, 7, A0, A0, T5);
    rv_op(0, 6, A0, A0, T4);
    store(ty);
    rv_mv(A0, T6);
    return;
  }

  if (is_void && is_int32(ty) && is_leaf(rhs) && is_stable_lvalue(lhs)) {
    int r = operand(rhs, A0);
    store_reg(r, ty, gen_lvalue(lhs));
    return;
  }

  Addr a = gen_lvalue(lhs);
  if (a.sym < 0 && a.base == A0) {
    push();
    gen_expr(rhs);
    a.base = pop_reg(T2);
  } else {
    gen_expr(rhs);
  }
  store_to(ty, a);
}

static bool has_stmt_expr(Node *node) {
  if (!node)
    return false;
  if (node->kind == ND_STMT_EXPR)
    return true;
  if (has_stmt_expr(node->lhs) || has_stmt_expr(node->rhs))
    return true;
  if (is_ctl_node(node))
    return has_stmt_expr(node->cond) || has_stmt_expr(node->then) || has_stmt_expr(node->els);
  if (node->kind == ND_FUNCALL)
    for (Node *n = node->args; n; n = n->next)
      if (has_stmt_expr(n))
        return true;
  return false;
}

// No code beyond reading variables.
static bool is_pure(Node *node) {
  int64_t k;
  switch (node->kind) {
  case ND_NUM:
  case ND_VAR:
    return true;
  case ND_CAST:
    return is_pure(node->lhs);
  default:
    return const_int(node, &k);
  }
}

// Evaluates node for its side effects only.
static void gen_void(Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
  case ND_NUM:
  case ND_VAR:
    return;
  case ND_CAST:
    gen_void(node->lhs);
    return;
  case ND_COMMA: {
    Node *l = node->lhs, *r = node->rhs;
    // `T x = e;` is memzero x, x = e: a scalar needs only the assignment.
    if (l->kind == ND_MEMZERO && r->kind == ND_ASSIGN && r->lhs->kind == ND_VAR &&
        r->lhs->var == l->var && !is_aggr(l->var->ty) && l->var->ty->kind != TY_ARRAY) {
      gen_void(r);
      return;
    }
    gen_void(l);
    gen_void(r);
    return;
  }
  case ND_ADD:
  case ND_SUB:
    if (is_pure(node->rhs)) {
      gen_void(node->lhs);
      return;
    }
    break;
  case ND_ASSIGN:
    gen_assign(node, true);
    return;
  case ND_MEMZERO:
    zero_local(node->var);
    return;
  default:
    break;
  }
  gen_expr(node);
}

static void gen_expr(Node *node) {
  check_stack(node->tok);
  int64_t k;
  if (node->kind != ND_NUM && const_int(node, &k)) {
    rv_li(A0, (int32_t)(uint32_t)k);
    if (is_i64(node->ty))
      rv_li(A1, (int32_t)(uint32_t)((uint64_t)k >> 32));
    return;
  }

  switch (node->kind) {
  case ND_NULL_EXPR:
    return;
  case ND_NUM: {
    if (node->ty->kind == TY_FLOAT) {
      gen_leaf(node, is_flt(node->ty) ? FA0 : A0);
    } else if (is_dbl(node->ty)) {
      double d = node->fval;
      uint64_t u;
      memcpy(&u, &d, 8);
      rv_li(A0, (int32_t)(uint32_t)u);
      rv_li(A1, (int32_t)(uint32_t)(u >> 32));
    } else if (is_i64(node->ty)) {
      rv_li(A0, (int32_t)(uint32_t)node->val);
      rv_li(A1, (int32_t)(uint32_t)((uint64_t)node->val >> 32));
    } else {
      rv_li(A0, (int32_t)(uint32_t)node->val);
    }
    return;
  }
  case ND_NEG:
    if (is_int32(node->ty) && is_int32(node->lhs->ty)) {
      rv_sub(A0, ZERO, gen_value(node->lhs, A0));
      return;
    }
    gen_expr(node->lhs);
    if (is_flt(node->ty)) {
      rv_fp(0x10, FA0, FA0, 1, FA0);  // fsgnjn.s fa0, fa0, fa0
    } else if (is_sflt(node->ty)) {
      rv_li(T0, (int32_t)0x80000000u);
      rv_op(0, 4, A0, A0, T0);
    } else if (is_dbl(node->ty)) {
      rv_li(T0, (int32_t)0x80000000u);
      rv_op(0, 4, A1, A1, T0);
    } else if (is_i64(node->ty)) {
      snez(T0, A0);
      rv_sub(A0, ZERO, A0);
      rv_sub(A1, ZERO, A1);
      rv_sub(A1, A1, T0);
    } else {
      rv_sub(A0, ZERO, A0);
    }
    return;
  case ND_VAR:
    if (node->var->reg) {
      if (is_flt(node->ty))
        fmv_s(FA0, node->var->reg);
      else
        rv_mv(A0, node->var->reg);
      return;
    }
    load_from(node->ty, gen_lvalue(node));
    return;
  case ND_MEMBER: {
    load_from(node->ty, gen_lvalue(node));
    Member *mem = node->member;
    if (mem->is_bitfield) {
      if (mem->ty->size > 4)
        error_tok(node->tok, "long long bit-fields are not supported");
      slli(A0, A0, 32 - mem->bit_width - mem->bit_offset);
      if (mem->ty->is_unsigned)
        srli(A0, A0, 32 - mem->bit_width);
      else
        srai(A0, A0, 32 - mem->bit_width);
    }
    return;
  }
  case ND_DEREF:
    load_from(node->ty, gen_lvalue(node));
    return;
  case ND_ADDR:
    gen_addr(node->lhs);
    return;
  case ND_ASSIGN:
    gen_assign(node, false);
    return;
  case ND_STMT_EXPR:
    for (Node *n = node->body; n; n = n->next) {
      if (!n->next && n->kind == ND_EXPR_STMT) {
        if (n->tok)
          add_line(n->tok);
        gen_expr(n->lhs);
      } else {
        gen_stmt(n);
      }
    }
    return;
  case ND_COMMA:
    gen_void(node->lhs);
    gen_expr(node->rhs);
    return;
  case ND_CAST:
    gen_expr(node->lhs);
    cast(node->lhs->ty, node->ty);
    return;
  case ND_MEMZERO:
    zero_local(node->var);
    return;
  case ND_COND: {
    int els = asm_new_label(), end = asm_new_label();
    gen_cond(node->cond, els, false);
    gen_expr(node->then);
    rv_jump(end);
    asm_bind(els);
    gen_expr(node->els);
    asm_bind(end);
    return;
  }
  case ND_NOT:
    if (is_int32(node->lhs->ty)) {
      seqz(A0, gen_value(node->lhs, A0));
      return;
    }
    gen_expr(node->lhs);
    to_bool(node->lhs->ty);
    xori(A0, A0, 1);
    return;
  case ND_BITNOT:
    if (is_int32(node->ty)) {
      xori(A0, gen_value(node->lhs, A0), -1);
      return;
    }
    gen_expr(node->lhs);
    xori(A0, A0, -1);
    if (is_i64(node->ty))
      xori(A1, A1, -1);
    return;
  case ND_LOGAND:
  case ND_LOGOR: {
    int f = asm_new_label(), end = asm_new_label();
    gen_cond(node, f, false);
    rv_li(A0, 1);
    rv_jump(end);
    asm_bind(f);
    rv_li(A0, 0);
    asm_bind(end);
    return;
  }
  case ND_FUNCALL:
    gen_funcall(node);
    return;
  case ND_LABEL_VAL:
    rv_label_addr(A0, label_of(node->unique_label));
    return;
  case ND_CAS: {
    gen_expr(node->cas_addr);
    push();
    gen_expr(node->cas_new);
    push();
    gen_expr(node->cas_old);
    rv_mv(T3, A0);
    load(node->cas_old->ty->base);
    pop(T1);
    pop(T2);
    if (node->cas_addr->ty->base->size != 4)
      error_tok(node->tok, "atomic operations are only supported on 32-bit types");
    int retry = asm_new_label(), fail = asm_new_label(), end = asm_new_label();
    asm_bind(retry);
    amo(0x02, T4, ZERO, T2);  // lr.w.aqrl t4, (t2)
    rv_branch(BNE, T4, A0, fail);
    amo(0x03, T5, T1, T2);    // sc.w.aqrl t5, t1, (t2)
    rv_branch(BNE, T5, ZERO, retry);
    rv_li(A0, 1);
    rv_jump(end);
    asm_bind(fail);
    rv_store(T4, T3, 0, 4);
    rv_li(A0, 0);
    asm_bind(end);
    return;
  }
  case ND_EXCH:
    gen_expr(node->lhs);
    push();
    gen_expr(node->rhs);
    pop(T2);
    if (node->lhs->ty->base->size != 4)
      error_tok(node->tok, "atomic operations are only supported on 32-bit types");
    amo(0x01, A0, A0, T2);  // amoswap.w.aqrl a0, a0, (t2)
    return;
  default:
    gen_binary(node);
  }
}

//
// Statements
//

static void gen_stmt(Node *node) {
  check_stack(node->tok);
  if (node->tok)
    add_line(node->tok);

  switch (node->kind) {
  case ND_IF: {
    int els = asm_new_label();
    gen_cond(node->cond, els, false);
    gen_stmt(node->then);
    if (node->els) {
      int end = asm_new_label();
      rv_jump(end);
      asm_bind(els);
      gen_stmt(node->els);
      asm_bind(end);
    } else {
      asm_bind(els);
    }
    return;
  }
  case ND_FOR: {
    int brk = label_of(node->brk_label);
    int body = asm_new_label();
    if (node->init)
      gen_stmt(node->init);
    // The test is at the bottom (and, as a guard, before the first iteration) unless it
    // cannot be duplicated.
    bool rotate = node->cond && !has_stmt_expr(node->cond);
    if (rotate)
      gen_cond(node->cond, brk, false);
    asm_bind(body);
    if (node->cond && !rotate)
      gen_cond(node->cond, brk, false);
    gen_stmt(node->then);
    asm_bind(label_of(node->cont_label));
    if (node->inc)
      gen_void(node->inc);
    if (rotate)
      gen_cond(node->cond, body, true);
    else
      rv_jump(body);
    asm_bind(brk);
    return;
  }
  case ND_DO: {
    int begin = asm_new_label();
    asm_bind(begin);
    gen_stmt(node->then);
    asm_bind(label_of(node->cont_label));
    gen_cond(node->cond, begin, true);
    asm_bind(label_of(node->brk_label));
    return;
  }
  case ND_SWITCH: {
    bool wide = is_i64(node->cond->ty);
    int r = A0;
    if (wide)
      gen_expr(node->cond);
    else
      r = gen_value(node->cond, A0);
    for (Node *n = node->cases; n; n = n->case_next) {
      int l = label_of(n->label);
      if (wide) {
        if (n->begin != n->end)
          error_tok(n->tok, "case ranges on long long are not supported");
        rv_li(T0, (int32_t)(uint32_t)n->begin);
        rv_li(T1, (int32_t)(uint32_t)((uint64_t)n->begin >> 32));
        rv_op(0, 4, T0, T0, A0);
        rv_op(0, 4, T1, T1, A1);
        rv_op(0, 6, T0, T0, T1);
        rv_branch(BEQ, T0, ZERO, l);
      } else if (n->begin == n->end) {
        if (n->begin) {
          rv_li(T0, (int32_t)n->begin);
          rv_branch(BEQ, r, T0, l);
        } else {
          rv_branch(BEQ, r, ZERO, l);
        }
      } else {
        rv_li(T0, (int32_t)n->begin);
        rv_sub(T1, r, T0);
        rv_li(T0, (int32_t)(n->end - n->begin));
        rv_branch(BGEU, T0, T1, l);
      }
    }
    if (node->default_case)
      rv_jump(label_of(node->default_case->label));
    else
      rv_jump(label_of(node->brk_label));
    gen_stmt(node->then);
    asm_bind(label_of(node->brk_label));
    return;
  }
  case ND_CASE:
    asm_bind(label_of(node->label));
    gen_stmt(node->lhs);
    return;
  case ND_BLOCK:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(n);
    return;
  case ND_PACKED: {
    CcFnMark m = cc_fn_mark();
    gen_stmt(cc_unpack(node));
    cc_node_release(m);
    return;
  }
  case ND_GOTO:
    rv_jump(label_of(node->unique_label));
    return;
  case ND_GOTO_EXPR:
    gen_expr(node->lhs);
    jalr(ZERO, A0);
    return;
  case ND_LABEL:
    asm_bind(label_of(node->unique_label));
    gen_stmt(node->lhs);
    return;
  case ND_RETURN:
    if (node->lhs) {
      gen_expr(node->lhs);
      Type *ty = node->lhs->ty;
      if (is_aggr(ty)) {
        if (ty->size <= 8) {
          if (ty->size > 4)
            load_bytes(A1, A0, 4, ty->size - 4, ty->align);
          load_bytes(A0, A0, 0, ty->size < 4 ? ty->size : 4, ty->align);
        } else {
          Obj *p = current_fn->params;  // the hidden pointer
          if (p->reg)
            rv_mv(T2, p->reg);
          else
            rv_load(T2, S0, p->offset, 4, 0);
          copy_mem(T2, A0, ty->size, ty->align);
          rv_mv(A0, T2);
        }
      }
    }
    rv_jump(return_label);
    return;
  case ND_EXPR_STMT:
    gen_void(node->lhs);
    return;
  case ND_ASM:
    error_tok(node->tok, "inline assembly is not supported");
  default:
    error_tok(node->tok, "invalid statement");
  }
}

//
// Register allocation
//

static bool fn_setjmp, fn_alloca;

static void mark_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    node->var->addr_taken = true;
    return;
  case ND_MEMBER:
    mark_addr(node->lhs);
    return;
  case ND_COMMA:
    mark_addr(node->rhs);
    return;
  default:
    return;
  }
}

static void scan(Node *node, int w);

static void scan_list(Node *node, int w) {
  for (; node; node = node->next)
    scan(node, w);
}

// Counts the uses of local variables (weighted by loop nesting) and finds the ones whose
// address is taken.
static void scan(Node *node, int w) {
  if (!node)
    return;
  switch (node->kind) {
  case ND_VAR:
    if (node->var->is_local)
      node->var->uses += w;
    break;
  case ND_PACKED: {
    CcFnMark m = cc_fn_mark();
    scan(cc_unpack(node), w);
    cc_node_release(m);
    return;
  }
  case ND_ADDR:
    mark_addr(node->lhs);
    break;
  case ND_ASSIGN:
    if (node->lhs->kind != ND_VAR)
      mark_addr(node->lhs);
    break;
  case ND_FUNCALL:
    if (node->lhs->kind == ND_VAR) {
      char *name = node->lhs->var->name;
      if (!strcmp(name, "setjmp") || !strcmp(name, "_setjmp") || !strcmp(name, "sigsetjmp") ||
          !strcmp(name, "__builtin_setjmp"))
        fn_setjmp = true;
      if (!strcmp(name, "alloca"))
        fn_alloca = true;
    }
    break;
  case ND_FOR:
  case ND_DO:
    scan(node->init, w);
    if (w < (1 << 24))
      w *= 8;
    scan(node->cond, w);
    scan(node->then, w);
    scan(node->inc, w);
    return;
  default:
    break;
  }
  scan(node->lhs, w);
  scan(node->rhs, w);
  if (is_ctl_node(node)) {
    scan(node->cond, w);
    scan(node->then, w);
    scan(node->els, w);  // ND_FOR and ND_DO returned above; a switch's cases are inside then
  } else if (node->kind == ND_BLOCK || node->kind == ND_STMT_EXPR) {
    scan_list(node->body, w);
  } else if (node->kind == ND_FUNCALL) {
    scan_list(node->args, w);
  } else if (node->kind == ND_CAS) {
    scan(node->cas_addr, w);
    scan(node->cas_old, w);
    scan(node->cas_new, w);
  }
}

static void alloc_regs(Obj *fn) {
  for (Obj *var = fn->locals; var; var = var->next) {
    var->uses = 0;
    var->reg = 0;
    var->addr_taken = false;
  }
  fn_setjmp = fn_alloca = false;
  scan(fn->body, 1);
  nvar_x = nvar_f = 0;
  if (fn_setjmp)
    return;
  for (;;) {
    Obj *best = NULL;
    for (Obj *var = fn->locals; var; var = var->next) {
      if (var->reg || var->addr_taken || var->is_byref || var->uses < 2 || !is_scalar32(var->ty) ||
          var == fn->alloca_bottom || var == fn->va_area)
        continue;
      if (is_flt(var->ty) ? nvar_f == MAX_VAR_REGS : nvar_x == MAX_VAR_REGS)
        continue;
      if (!best || var->uses > best->uses)
        best = var;
    }
    if (!best)
      return;
    best->reg = is_flt(best->ty) ? fsregs[nvar_f++] : sregs[nvar_x++];
  }
}

//
// Functions and data
//

static int popcount(uint32_t v) {
  int n = 0;
  for (; v; v &= v - 1)
    n++;
  return n;
}

static void emit_function(Obj *fn) {
  current_fn = fn;
  int va = fn->ty->is_variadic ? 32 : 0;

  int sym_idx = sym_index(fn->name);  // may grow out->syms
  CcSym *sym = &out->syms[sym_idx];
  sym->sect = CC_TEXT;
  sym->offset = asm_pc();
  sym->is_global = !fn->is_static;
  sym->line = fn->tok ? fn->tok->line_no : 0;
  uint32_t start = sym->offset;

  // Parameter locations
  int nparams = 0;
  for (Obj *p = fn->params; p; p = p->next)
    nparams++;
  Loc *locs = cc_fn_calloc((nparams + 1) * sizeof(Loc));
  AbiState st = {0};
  int i = 0;
  for (Obj *p = fn->params; p; p = p->next, i++) {
    locs[i] = classify(&st, p->ty, false);
    p->is_byref = is_byref_type(p->ty);
  }

  alloc_regs(fn);
  uint32_t var_x = 0, var_f = 0;
  for (i = 0; i < nvar_x; i++)
    var_x |= 1u << sregs[i];
  for (i = 0; i < nvar_f; i++)
    var_f |= 1u << fsregs[i];

  // Two passes: the first saves every callee-saved register and finds the ones used.
  uint32_t save_x = 0, save_f = 0;
  for (i = 0; i < 11; i++)
    save_x |= 1u << sregs[i];
  for (i = 0; i < 12; i++)
    save_f |= 1u << fsregs[i];
  int nlines = out->nlines;
  CcLine last_line = nlines ? out->lines[nlines - 1] : (CcLine){0};
  int line0 = cur_line;
  asm_begin_function();

  for (int pass = 0; pass < 2; pass++) {
    label_map = (HashMap){.in_fn = true};
    depth = 0;
    nslots = 0;
    tmp_x = tmp_f = 0;
    used_x = var_x;
    used_f = var_f;
    int nsave = popcount(save_x) + popcount(save_f);

    // Local slots (parameters included; stack-passed ones are overridden below)
    int bottom = va + 8 + 4 * nsave;
    for (Obj *var = fn->locals; var; var = var->next) {
      if (var->reg)
        continue;
      int size = var->is_byref ? 4 : var->ty->size;
      int align = var->align > 4 ? var->align : 4;
      bottom += align_to(size, 4);
      bottom = align_to(bottom, align);
      var->offset = -bottom;
    }
    int frame = align_to(bottom, 16);

    // Prologue
    if (fn->tok)
      add_line(fn->tok);
    if (frame <= 2032) {
      rv_addi(SP, SP, -frame);
      rv_store(RA, SP, frame - va - 4, 4);
      rv_store(S0, SP, frame - va - 8, 4);
      for (int r = 0; r < 8 && va; r++)
        rv_store(A0 + r, SP, frame - 32 + 4 * r, 4);
      rv_addi(S0, SP, frame);
    } else {
      rv_addi(SP, SP, -(va + 8));
      rv_store(RA, SP, 4, 4);
      rv_store(S0, SP, 0, 4);
      for (int r = 0; r < 8 && va; r++)
        rv_store(A0 + r, SP, 8 + 4 * r, 4);
      rv_addi(S0, SP, va + 8);
      rv_addr(SP, SP, -(frame - va - 8));
    }
    int k = 0;
    for (int r = 0; r < 32; r++)
      if (save_x >> r & 1)
        rv_store(r, S0, -va - 12 - 4 * k++, 4);
    for (int r = 0; r < 32; r++)
      if (save_f >> r & 1)
        rv_fsw(r, S0, -va - 12 - 4 * k++);
    if (fn_alloca)
      rv_store(SP, S0, fn->alloca_bottom->offset, 4);

    i = 0;
    for (Obj *p = fn->params; p; p = p->next, i++) {
      Loc l = locs[i];
      if (p->reg) {
        bool f = is_flt(p->ty);
        switch (l.kind) {
        case L_GPR:
          if (f) {
            fmv_w_x(p->reg, A0 + l.reg);
          } else {
            rv_mv(p->reg, A0 + l.reg);
            narrow_reg(p->reg, type_id(p->ty));
          }
          break;
        case L_FPR:
          fmv_s(p->reg, FA0 + l.reg);
          break;
        case L_STACK:
          p->offset = l.off;
          if (f)
            rv_flw(p->reg, S0, l.off);
          else
            rv_load(p->reg, S0, l.off, p->ty->size, p->ty->is_unsigned);
          break;
        default:
          error("internal error: register parameter");
        }
        continue;
      }
      switch (l.kind) {
      case L_GPR:
        rv_store(A0 + l.reg, S0, p->offset, 4);
        break;
      case L_GPR2:
        rv_store(A0 + l.reg, S0, p->offset, 4);
        rv_store(A0 + l.reg + 1, S0, p->offset + 4, 4);
        break;
      case L_FPR:
        rv_fsw(FA0 + l.reg, S0, p->offset);
        break;
      case L_SPLIT:
        rv_store(A7, S0, p->offset, 4);
        rv_load(T0, S0, l.off, 4, 0);
        rv_store(T0, S0, p->offset + 4, 4);
        break;
      case L_STACK:
        p->offset = l.off;
        break;
      }
    }
    if (fn->va_area)
      fn->va_area->offset = st.gp < 8 ? -32 + 4 * st.gp : st.stack;

    return_label = asm_new_label();
    gen_stmt(fn->body);
    if (depth != 0 || nslots != 0)
      error("internal error: stack depth %d in %s", depth, fn->name);

    // main() without a return statement returns 0.
    if (!strcmp(fn->name, "main"))
      rv_li(A0, 0);

    // Epilogue (never touches memory below sp)
    asm_bind(return_label);
    rv_load(RA, S0, -va - 4, 4, 0);
    k = 0;
    for (int r = 0; r < 32; r++)
      if (save_x >> r & 1)
        rv_load(r, S0, -va - 12 - 4 * k++, 4, 0);
    for (int r = 0; r < 32; r++)
      if (save_f >> r & 1)
        rv_flw(r, S0, -va - 12 - 4 * k++);
    rv_load(T0, S0, -va - 8, 4, 0);
    rv_mv(SP, S0);
    rv_mv(S0, T0);
    jalr(ZERO, RA);

    if (pass == 0) {
      save_x = used_x;
      save_f = used_f;
      asm_rollback();
      out->nlines = nlines;
      if (nlines)
        out->lines[nlines - 1] = last_line;
      cur_line = line0;
    }
  }
  asm_end_function();
  sym = &out->syms[sym_idx];
  sym->size = asm_pc() - start;
}

static bool all_zero(char *p, int n) {
  for (int i = 0; i < n; i++)
    if (p[i])
      return false;
  return true;
}

// String literals go in .data as soon as they are parsed. Each is reached as an offset from one
// local symbol at the start of .data, so it takes no symbol of its own, and its Obj (whose name
// and offset these are) need not outlive the Nodes that use it (see new_string_literal()).
void codegen_string(Obj *var, char *data) {
  static char name[] = ".L..strings";
  int s = sym_index(name);
  out->syms[s].sect = CC_DATA;
  out->syms[s].is_global = 0;
  CcSection *sect = &out->sect[CC_DATA];
  sect_align(sect, var->align);
  var->name = name;
  var->offset = sect->size;
  sect_append(sect, data, var->ty->size);
}

static void emit_data(Obj *prog) {
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function || !var->is_definition)
      continue;
    if (var->is_tls)
      error_tok(var->tok, "thread-local variables are not supported");

    int s = sym_index(var->name);
    CcSym *sym = &out->syms[s];
    int align = var->align > 0 ? var->align : 1;
    int size = var->ty->size;
    sym->size = size;
    sym->is_global = !var->is_static;
    if (var->tok)
      sym->line = var->tok->line_no;

    if (var->is_tentative && !var->is_static && opt_fcommon) {
      sym->is_common = 1;
      sym->sect = CC_BSS;
      sym->offset = align;  // commons carry their alignment here; the linker places them
      continue;
    }

    if (!var->init_data || (!var->rel && all_zero(var->init_data, size))) {
      CcSection *bss = &out->sect[CC_BSS];
      if ((uint32_t)align > bss->align)
        bss->align = align;
      bss->size = align_to(bss->size, align);
      sym->sect = CC_BSS;
      sym->offset = bss->size;
      bss->size += size;
      continue;
    }

    CcSection *data = &out->sect[CC_DATA];
    sect_align(data, align);
    sym->sect = CC_DATA;
    sym->offset = data->size;
    sect_append(data, var->init_data, size);
    for (Relocation *rel = var->rel; rel; rel = rel->next) {
      int target = sym_index(rel->label);
      // sym_index may have grown the table
      data_reloc(out->syms[s].offset + rel->offset, target, rel->addend);
    }
  }
}

void codegen_begin(CcObject *o) {
  out = o;
  sym_map = (HashMap){0};
  file_map = (HashMap){0};
  cur_line = 0;
  asm_init(o);
  for (int i = CC_RODATA; i < CC_NSECT; i++)
    if (!o->sect[i].align)
      o->sect[i].align = 4;
}

// Compiles a function as soon as it has been parsed; its body is dropped afterwards.
void codegen_function(Obj *fn) {
  emit_function(fn);
}

// Static inline functions kept their bodies until it was known which are used.
void codegen_end(Obj *prog) {
  emit_data(prog);
  for (Obj *fn = prog; fn; fn = fn->next)
    if (fn->is_function && fn->body && fn->is_live)
      emit_function(fn);
}
