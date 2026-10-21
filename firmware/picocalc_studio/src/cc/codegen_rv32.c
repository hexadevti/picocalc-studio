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
static int depth;          // bytes on the temporary push area
static int return_label;
static HashMap label_map;  // chibicc label names -> asm labels (per function)
static HashMap sym_map;    // symbol name -> index + 1
static HashMap file_map;   // file name -> index + 1
static int cur_line;

static void gen_expr(Node *node);
static void gen_stmt(Node *node);

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
    out->syms = cc_realloc(out->syms, sizeof(CcSym) * out->capsyms, sizeof(CcSym) * cap);
    out->capsyms = cap;
  }
  int n = out->nsyms++;
  out->syms[n] = (CcSym){.name = name, .sect = -1, .is_global = 1, .line = cur_line};
  hashmap_put(&sym_map, name, (void *)(intptr_t)(n + 1));
  return n;
}

static void data_reloc(uint32_t offset, int sym, int32_t addend) {
  if (out->nrelocs == out->caprelocs) {
    int cap = out->caprelocs ? out->caprelocs * 2 : 64;
    out->relocs = cc_realloc(out->relocs, sizeof(CcReloc) * out->caprelocs, sizeof(CcReloc) * cap);
    out->caprelocs = cap;
  }
  out->relocs[out->nrelocs++] = (CcReloc){CC_DATA, offset, CC_R_ABS32, sym, addend, 0};
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
      out->files = cc_realloc(out->files, sizeof(char *) * out->capfiles, sizeof(char *) * cap);
      out->capfiles = cap;
    }
    out->files[out->nfiles++] = name;
    f = out->nfiles;
    hashmap_put(&file_map, name, (void *)f);
  }
  cur_line = tok->line_no;

  uint32_t pc = asm_pc();
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
    out->lines = cc_realloc(out->lines, sizeof(CcLine) * out->caplines, sizeof(CcLine) * cap);
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

static bool is_flt(Type *ty) { return ty->kind == TY_FLOAT; }
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

static void push(void) {
  rv_addi(SP, SP, -4);
  rv_store(A0, SP, 0, 4);
  depth += 4;
}

static void pop(int reg) {
  rv_load(reg, SP, 0, 4, 0);
  rv_addi(SP, SP, 4);
  depth -= 4;
}

static void push_val(Type *ty) {
  if (is_flt(ty)) {
    rv_addi(SP, SP, -4);
    rv_fsw(FA0, SP, 0);
    depth += 4;
  } else if (is_wide(ty)) {
    rv_addi(SP, SP, -8);
    rv_store(A0, SP, 0, 4);
    rv_store(A1, SP, 4, 4);
    depth += 8;
  } else {
    push();
  }
}

// Pops a value of type ty into t0 (+t1) or ft0.
static void pop_val(Type *ty) {
  if (is_flt(ty)) {
    rv_flw(FT0, SP, 0);
    rv_addi(SP, SP, 4);
    depth -= 4;
  } else if (is_wide(ty)) {
    rv_load(T0, SP, 0, 4, 0);
    rv_load(T1, SP, 4, 4, 0);
    rv_addi(SP, SP, 8);
    depth -= 8;
  } else {
    pop(T0);
  }
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

// Loads a value of type ty from the address in a0.
static void load(Type *ty) {
  switch (ty->kind) {
  case TY_ARRAY:
  case TY_STRUCT:
  case TY_UNION:
  case TY_FUNC:
  case TY_VLA:
    return;
  case TY_FLOAT:
    rv_flw(FA0, A0, 0);
    return;
  case TY_DOUBLE:
  case TY_LDOUBLE:
  case TY_LONG:
    rv_load(A1, A0, 4, 4, 0);
    rv_load(A0, A0, 0, 4, 0);
    return;
  default:
    rv_load(A0, A0, 0, ty->size, ty->is_unsigned);
  }
}

// Stores the accumulator to the address on top of the push area.
static void store(Type *ty) {
  pop(T2);
  switch (ty->kind) {
  case TY_STRUCT:
  case TY_UNION:
    copy_mem(T2, A0, ty->size, ty->align);
    return;
  case TY_FLOAT:
    rv_fsw(FA0, T2, 0);
    return;
  case TY_DOUBLE:
  case TY_LDOUBLE:
  case TY_LONG:
    rv_store(A0, T2, 0, 4);
    rv_store(A1, T2, 4, 4);
    return;
  default:
    rv_store(A0, T2, 0, ty->size);
  }
}

// Zeroes a local variable (slots are 4-byte aligned and padded to 4 bytes).
static void zero_local(Obj *var) {
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

static void gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR: {
    Obj *var = node->var;
    if (var->is_local) {
      if (var->ty->kind == TY_VLA || var->is_byref)
        rv_load(A0, S0, var->offset, 4, 0);
      else
        rv_addr(A0, S0, var->offset);
      return;
    }
    if (var->is_tls)
      error_tok(node->tok, "thread-local variables are not supported");
    rv_sym_addr(A0, sym_index(var->name), 0, cur_line);
    return;
  }
  case ND_DEREF:
    gen_expr(node->lhs);
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
    gen_addr(node->rhs);
    return;
  case ND_MEMBER:
    gen_addr(node->lhs);
    if (node->member->offset)
      rv_addr(A0, A0, node->member->offset);
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

// Re-extends a0 to the canonical 32-bit form of an integer type.
static void narrow(int id) {
  switch (id) {
  case I8:
    slli(A0, A0, 24);
    srai(A0, A0, 24);
    break;
  case U8:
    andi(A0, A0, 0xff);
    break;
  case I16:
    slli(A0, A0, 16);
    srai(A0, A0, 16);
    break;
  case U16:
    slli(A0, A0, 16);
    srli(A0, A0, 16);
    break;
  }
}

// a0 = (accumulator != 0)
static void to_bool(Type *ty) {
  if (is_flt(ty)) {
    fmv_x_w(T0, FA0);
    slli(T0, T0, 1);  // ignore the sign: -0.0 is false
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
  if (is_flt(ty)) {
    fmv_x_w(T0, FA0);
    slli(T0, T0, 1);
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
    else if (t2 == U32)
      rv_fp(0x60, 1, FA0, 1, A0);  // fcvt.wu.s a0, fa0, rtz
    else {
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
  if (t2 == F32)
    rv_fp(0x68, u ? 1 : 0, A0, 7, FA0);  // fcvt.s.w[u] fa0, a0
  else
    libcall(u ? "__floatunsidf" : "__floatsidf");
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
  for (Type *t = fty->params; t; t = t->next)
    nparams++;
  int nargs = 0;
  for (Node *a = node->args; a; a = a->next)
    nargs++;

  Loc *locs = calloc(nargs + 1, sizeof(Loc));
  int *tmp = calloc(nargs + 1, sizeof(int));
  AbiState st = {0};
  bool sret = node->ret_buffer && node->ty->size > 8;
  if (sret)
    st.gp = 1;
  int i = 0;
  for (Node *a = node->args; a; a = a->next, i++)
    locs[i] = classify(&st, arg_type(a->ty), i >= nparams && fty->is_variadic);

  int base_depth = depth;

  // Evaluate the arguments left to right onto the push area.
  i = 0;
  for (Node *a = node->args; a; a = a->next, i++) {
    Type *ty = arg_type(a->ty);
    gen_expr(a);
    if (is_byref_type(ty)) {
      int sz = align_to(ty->size, 8);
      sz += (8 - (depth + sz) % 8) % 8;
      rv_addr(SP, SP, -sz);
      depth += sz;
      copy_mem(SP, A0, ty->size, ty->align);
      rv_mv(A0, SP);
      push();
    } else if (is_aggr(ty)) {
      int sz = align_to(ty->size, 4);
      rv_addi(SP, SP, -sz);
      depth += sz;
      copy_mem(SP, A0, ty->size, ty->align);
    } else {
      push_val(ty);
    }
    tmp[i] = depth;
  }

  bool direct = node->lhs->kind == ND_VAR && node->lhs->var->ty->kind == TY_FUNC &&
                !node->lhs->var->is_local;
  int fn_tmp = 0;
  if (!direct) {
    gen_expr(node->lhs);
    push();
    fn_tmp = depth;
  }

  // Outgoing stack arguments at the new sp, which is 16-byte aligned.
  int stack_bytes = align_to(st.stack, 4);
  int area = stack_bytes + (16 - (depth + stack_bytes) % 16) % 16;
  if (area) {
    rv_addr(SP, SP, -area);
    depth += area;
  }

  i = 0;
  for (Node *a = node->args; a; a = a->next, i++) {
    int src = depth - tmp[i];
    Loc l = locs[i];
    Type *ty = arg_type(a->ty);
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
  }

  if (sret)
    rv_addr(A0, S0, node->ret_buffer->offset);
  if (direct) {
    rv_call_sym(sym_index(node->lhs->var->name), cur_line);
  } else {
    rv_load(T6, SP, depth - fn_tmp, 4, 0);
    jalr(RA, T6);
  }

  rv_addr(SP, SP, depth - base_depth);
  depth = base_depth;

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

static void lt64(int alo, int ahi, int blo, int bhi, bool is_unsigned) {
  rv_op(0, is_unsigned ? 3 : 2, T2, ahi, bhi);  // hi <
  rv_op(0, 4, T3, ahi, bhi);
  seqz(T3, T3);                                 // hi ==
  rv_op(0, 3, T4, alo, blo);                    // lo <u
  rv_op(0, 7, T3, T3, T4);
  rv_op(0, 6, A0, T2, T3);
}

static void gen_binary(Node *node) {
  Type *ty = node->lhs->ty;

  if (node->kind == ND_SHL || node->kind == ND_SHR) {
    gen_expr(node->rhs);  // only the low word of the count matters
    if (is_flt(node->rhs->ty) || is_dbl(node->rhs->ty))
      error_tok(node->tok, "invalid operand");
    push();
    gen_expr(node->lhs);
    pop(T0);
    if (is_i64(ty)) {
      rv_mv(A2, T0);
      libcall(node->kind == ND_SHL ? "__ashldi3" : ty->is_unsigned ? "__lshrdi3" : "__ashrdi3");
    } else if (node->kind == ND_SHL) {
      rv_op(0, 1, A0, A0, T0);
    } else {
      rv_op(ty->is_unsigned ? 0 : 0x20, 5, A0, A0, T0);
    }
    return;
  }

  gen_expr(node->rhs);
  push_val(node->rhs->ty);
  gen_expr(node->lhs);
  pop_val(node->rhs->ty);

  if (is_flt(ty)) {
    switch (node->kind) {
    case ND_ADD: rv_fp(0x00, FT0, FA0, 7, FA0); return;
    case ND_SUB: rv_fp(0x04, FT0, FA0, 7, FA0); return;
    case ND_MUL: rv_fp(0x08, FT0, FA0, 7, FA0); return;
    case ND_DIV: rv_fp(0x0c, FT0, FA0, 7, FA0); return;
    case ND_EQ: rv_fp(0x50, FT0, FA0, 2, A0); return;
    case ND_NE: rv_fp(0x50, FT0, FA0, 2, A0); xori(A0, A0, 1); return;
    case ND_LT: rv_fp(0x50, FT0, FA0, 1, A0); return;
    case ND_LE: rv_fp(0x50, FT0, FA0, 0, A0); return;
    default: error_tok(node->tok, "invalid expression");
    }
  }

  if (is_dbl(ty)) {
    rv_mv(A2, T0);
    rv_mv(A3, T1);
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
    case ND_DIV:
    case ND_MOD:
      rv_mv(A2, T0);
      rv_mv(A3, T1);
      if (node->kind == ND_DIV)
        libcall(u ? "__udivdi3" : "__divdi3");
      else
        libcall(u ? "__umoddi3" : "__moddi3");
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

  bool u = ty->is_unsigned;
  switch (node->kind) {
  case ND_ADD: rv_add(A0, A0, T0); return;
  case ND_SUB: rv_sub(A0, A0, T0); return;
  case ND_MUL: rv_op(1, 0, A0, A0, T0); return;
  case ND_DIV: rv_op(1, u ? 5 : 4, A0, A0, T0); return;
  case ND_MOD: rv_op(1, u ? 7 : 6, A0, A0, T0); return;
  case ND_BITAND: rv_op(0, 7, A0, A0, T0); return;
  case ND_BITOR: rv_op(0, 6, A0, A0, T0); return;
  case ND_BITXOR: rv_op(0, 4, A0, A0, T0); return;
  case ND_EQ: rv_sub(A0, A0, T0); seqz(A0, A0); return;
  case ND_NE: rv_sub(A0, A0, T0); snez(A0, A0); return;
  case ND_LT: rv_op(0, u ? 3 : 2, A0, A0, T0); return;
  case ND_LE: rv_op(0, u ? 3 : 2, A0, T0, A0); xori(A0, A0, 1); return;
  default: error_tok(node->tok, "invalid expression");
  }
}

static void gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
    return;
  case ND_NUM: {
    if (is_flt(node->ty)) {
      float f = node->fval;
      uint32_t u;
      memcpy(&u, &f, 4);
      rv_li(A0, u);
      fmv_w_x(FA0, A0);
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
    gen_expr(node->lhs);
    if (is_flt(node->ty)) {
      rv_fp(0x10, FA0, FA0, 1, FA0);  // fsgnjn.s fa0, fa0, fa0
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
    gen_addr(node);
    load(node->ty);
    return;
  case ND_MEMBER: {
    gen_addr(node);
    load(node->ty);
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
    gen_expr(node->lhs);
    load(node->ty);
    return;
  case ND_ADDR:
    gen_addr(node->lhs);
    return;
  case ND_ASSIGN:
    gen_addr(node->lhs);
    push();
    gen_expr(node->rhs);
    if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield) {
      Member *mem = node->lhs->member;
      if (mem->ty->size > 4)
        error_tok(node->tok, "long long bit-fields are not supported");
      uint32_t mask = mem->bit_width == 32 ? 0xffffffffu : (1u << mem->bit_width) - 1;
      rv_mv(T6, A0);  // the new value is the result
      rv_li(T5, (int32_t)mask);
      rv_op(0, 7, T4, A0, T5);
      slli(T4, T4, mem->bit_offset);
      rv_load(A0, SP, 0, 4, 0);
      load(mem->ty);
      rv_li(T5, (int32_t)~(mask << mem->bit_offset));
      rv_op(0, 7, A0, A0, T5);
      rv_op(0, 6, A0, A0, T4);
      store(node->ty);
      rv_mv(A0, T6);
      return;
    }
    store(node->ty);
    return;
  case ND_STMT_EXPR:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(n);
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
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
    gen_expr(node->cond);
    branch_zero(node->cond->ty, els, true);
    gen_expr(node->then);
    rv_jump(end);
    asm_bind(els);
    gen_expr(node->els);
    asm_bind(end);
    return;
  }
  case ND_NOT:
    gen_expr(node->lhs);
    to_bool(node->lhs->ty);
    xori(A0, A0, 1);
    return;
  case ND_BITNOT:
    gen_expr(node->lhs);
    xori(A0, A0, -1);
    if (is_i64(node->ty))
      xori(A1, A1, -1);
    return;
  case ND_LOGAND:
  case ND_LOGOR: {
    bool is_and = node->kind == ND_LOGAND;
    int skip = asm_new_label(), end = asm_new_label();
    gen_expr(node->lhs);
    branch_zero(node->lhs->ty, skip, is_and);
    gen_expr(node->rhs);
    branch_zero(node->rhs->ty, skip, is_and);
    rv_li(A0, is_and ? 1 : 0);
    rv_jump(end);
    asm_bind(skip);
    rv_li(A0, is_and ? 0 : 1);
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
  if (node->tok)
    add_line(node->tok);

  switch (node->kind) {
  case ND_IF: {
    int els = asm_new_label(), end = asm_new_label();
    gen_expr(node->cond);
    branch_zero(node->cond->ty, els, true);
    gen_stmt(node->then);
    rv_jump(end);
    asm_bind(els);
    if (node->els)
      gen_stmt(node->els);
    asm_bind(end);
    return;
  }
  case ND_FOR: {
    int begin = asm_new_label();
    int brk = label_of(node->brk_label);
    if (node->init)
      gen_stmt(node->init);
    asm_bind(begin);
    if (node->cond) {
      gen_expr(node->cond);
      branch_zero(node->cond->ty, brk, true);
    }
    gen_stmt(node->then);
    asm_bind(label_of(node->cont_label));
    if (node->inc)
      gen_expr(node->inc);
    rv_jump(begin);
    asm_bind(brk);
    return;
  }
  case ND_DO: {
    int begin = asm_new_label();
    asm_bind(begin);
    gen_stmt(node->then);
    asm_bind(label_of(node->cont_label));
    gen_expr(node->cond);
    branch_zero(node->cond->ty, begin, false);
    asm_bind(label_of(node->brk_label));
    return;
  }
  case ND_SWITCH: {
    gen_expr(node->cond);
    bool wide = is_i64(node->cond->ty);
    for (Node *n = node->case_next; n; n = n->case_next) {
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
        rv_li(T0, (int32_t)n->begin);
        rv_branch(BEQ, A0, T0, l);
      } else {
        rv_li(T0, (int32_t)n->begin);
        rv_sub(T1, A0, T0);
        rv_li(T0, (int32_t)(n->end - n->begin));
        rv_branch(BGEU, T0, T1, l);
      }
    }
    if (node->default_case)
      rv_jump(label_of(node->default_case->label));
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
          rv_load(T2, S0, current_fn->params->offset, 4, 0);  // hidden pointer
          copy_mem(T2, A0, ty->size, ty->align);
          rv_mv(A0, T2);
        }
      }
    }
    rv_jump(return_label);
    return;
  case ND_EXPR_STMT:
    gen_expr(node->lhs);
    return;
  case ND_ASM:
    error_tok(node->tok, "inline assembly is not supported");
  default:
    error_tok(node->tok, "invalid statement");
  }
}

//
// Functions and data
//

static void emit_function(Obj *fn) {
  current_fn = fn;
  label_map = (HashMap){0};
  depth = 0;
  int va = fn->ty->is_variadic ? 32 : 0;

  CcSym *sym = &out->syms[sym_index(fn->name)];
  sym->sect = CC_TEXT;
  sym->offset = asm_pc();
  sym->is_global = !fn->is_static;
  sym->line = fn->tok ? fn->tok->line_no : 0;

  // Parameter locations
  int nparams = 0;
  for (Obj *p = fn->params; p; p = p->next)
    nparams++;
  Loc *locs = calloc(nparams + 1, sizeof(Loc));
  AbiState st = {0};
  int i = 0;
  for (Obj *p = fn->params; p; p = p->next, i++) {
    locs[i] = classify(&st, p->ty, false);
    p->is_byref = is_byref_type(p->ty);
  }

  // Local slots (parameters included; stack-passed ones are overridden below)
  int bottom = va + 8;
  for (Obj *var = fn->locals; var; var = var->next) {
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
  rv_addi(SP, SP, -(va + 8));
  rv_store(RA, SP, 4, 4);
  rv_store(S0, SP, 0, 4);
  for (int r = 0; r < 8 && va; r++)
    rv_store(A0 + r, SP, 8 + 4 * r, 4);
  rv_addi(S0, SP, va + 8);
  if (frame > va + 8)
    rv_addr(SP, SP, -(frame - va - 8));
  rv_store(SP, S0, fn->alloca_bottom->offset, 4);

  i = 0;
  for (Obj *p = fn->params; p; p = p->next, i++) {
    Loc l = locs[i];
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
  if (depth != 0)
    error("internal error: stack depth %d in %s", depth, fn->name);

  // main() without a return statement returns 0.
  if (!strcmp(fn->name, "main"))
    rv_li(A0, 0);

  // Epilogue (never touches memory below sp)
  asm_bind(return_label);
  rv_load(RA, S0, -va - 4, 4, 0);
  rv_load(T0, S0, -va - 8, 4, 0);
  rv_mv(SP, S0);
  rv_mv(S0, T0);
  jalr(ZERO, RA);
  asm_end_function();
  sym->size = asm_pc() - sym->offset;
}

static bool all_zero(char *p, int n) {
  for (int i = 0; i < n; i++)
    if (p[i])
      return false;
  return true;
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
      int target = sym_index(*rel->label);
      // sym_index may have grown the table
      data_reloc(out->syms[s].offset + rel->offset, target, rel->addend);
    }
  }
}

void codegen(Obj *prog, CcObject *o) {
  out = o;
  sym_map = (HashMap){0};
  file_map = (HashMap){0};
  cur_line = 0;
  asm_init(o);
  for (int i = CC_RODATA; i < CC_NSECT; i++)
    if (!o->sect[i].align)
      o->sect[i].align = 4;

  emit_data(prog);
  for (Obj *fn = prog; fn; fn = fn->next)
    if (fn->is_function && fn->is_definition && fn->is_live)
      emit_function(fn);
}
