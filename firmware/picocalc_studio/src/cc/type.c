#include "chibicc.h"

Type *ty_void = &(Type){.kind = TY_VOID, .size = 1, .align = 1};
Type *ty_bool = &(Type){.kind = TY_BOOL, .size = 1, .align = 1};

Type *ty_char = &(Type){.kind = TY_CHAR, .size = 1, .align = 1};
Type *ty_short = &(Type){.kind = TY_SHORT, .size = 2, .align = 2};
Type *ty_int = &(Type){.kind = TY_INT, .size = 4, .align = 4};
Type *ty_long = &(Type){.kind = TY_LONG, .size = 8, .align = 8};

Type *ty_uchar = &(Type){.kind = TY_CHAR, .size = 1, .align = 1, .is_unsigned = true};
Type *ty_ushort = &(Type){.kind = TY_SHORT, .size = 2, .align = 2, .is_unsigned = true};
Type *ty_uint = &(Type){.kind = TY_INT, .size = 4, .align = 4, .is_unsigned = true};
Type *ty_ulong = &(Type){.kind = TY_LONG, .size = 8, .align = 8, .is_unsigned = true};

Type *ty_float = &(Type){.kind = TY_FLOAT, .size = 4, .align = 4};
Type *ty_double = &(Type){.kind = TY_DOUBLE, .size = 8, .align = 8};
// long double is the same as double on this target (the RISC-V ABI says 16-byte quad;
// we do not support quad precision).
Type *ty_ldouble = &(Type){.kind = TY_DOUBLE, .size = 8, .align = 8};

// `long` is 32 bits and maps to the int types; TY_LONG is `long long`.
Type *ty_size_t = &(Type){.kind = TY_INT, .size = 4, .align = 4, .is_unsigned = true};
Type *ty_ptrdiff_t = &(Type){.kind = TY_INT, .size = 4, .align = 4};

static Type *new_type(TypeKind kind, int size, int align) {
  Type *ty = cc_type_calloc(sizeof(Type));
  ty->kind = kind;
  ty->size = size;
  ty->align = align;
  return ty;
}

bool is_integer(Type *ty) {
  TypeKind k = ty->kind;
  return k == TY_BOOL || k == TY_CHAR || k == TY_SHORT ||
         k == TY_INT  || k == TY_LONG || k == TY_ENUM;
}

bool is_flonum(Type *ty) {
  return ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE ||
         ty->kind == TY_LDOUBLE;
}

bool is_numeric(Type *ty) {
  return is_integer(ty) || is_flonum(ty);
}

bool is_compatible(Type *t1, Type *t2) {
  if (t1 == t2)
    return true;

  if (t1->origin)
    return is_compatible(t1->origin, t2);

  if (t2->origin)
    return is_compatible(t1, t2->origin);

  if (t1->kind != t2->kind)
    return false;

  switch (t1->kind) {
  case TY_CHAR:
  case TY_SHORT:
  case TY_INT:
  case TY_LONG:
    return t1->is_unsigned == t2->is_unsigned;
  case TY_FLOAT:
  case TY_DOUBLE:
  case TY_LDOUBLE:
    return true;
  case TY_PTR:
    return is_compatible(t1->base, t2->base);
  case TY_FUNC: {
    if (!is_compatible(t1->return_ty, t2->return_ty))
      return false;
    if (t1->is_variadic != t2->is_variadic)
      return false;

    Type *none = NULL;
    Type **p1 = t1->params ? t1->params : &none;
    Type **p2 = t2->params ? t2->params : &none;
    for (; *p1 && *p2; p1++, p2++)
      if (!is_compatible(*p1, *p2))
        return false;
    return !*p1 && !*p2;
  }
  case TY_ARRAY:
    if (!is_compatible(t1->base, t2->base))
      return false;
    return t1->array_len < 0 && t2->array_len < 0 &&
           t1->array_len == t2->array_len;
  }
  return false;
}

Type *copy_type(Type *ty) {
  Type *ret = cc_type_calloc(sizeof(Type));
  *ret = *ty;
  ret->origin = ty;
  return ret;
}

// A function type's parameter list as keep_type() keeps it.
static Type **keep_params(Type **params) {
  if (!params)
    return NULL;
  int n = 0;
  while (params[n])
    n++;
  Type **p = params;
  for (int i = 0; i < n; i++) {
    Type *k = keep_type(params[i]);
    if (p == params && (k != params[i] || cc_in_fn_arena(params))) {
      bool was = cc_fn_types(false);
      p = calloc(n + 1, sizeof(Type *));
      cc_fn_types(was);
      memcpy(p, params, sizeof(Type *) * n);
    }
    p[i] = k;
  }
  return p;
}

// ty, or a copy in the main arena if it (or a type it is made of) is in the function arena: for
// a global (a static local, a string literal) that outlives the function.
Type *keep_type(Type *ty) {
  if (!ty)
    return NULL;
  bool in_fn = cc_in_fn_arena(ty);
  Type *base = keep_type(ty->base), *origin = keep_type(ty->origin);
  Type *ret_ty = ty->kind == TY_FUNC ? keep_type(ty->return_ty) : NULL;
  Type **params = ty->kind == TY_FUNC ? keep_params(ty->params) : NULL;
  if (!in_fn && base == ty->base && origin == ty->origin &&
      (ty->kind != TY_FUNC || (ret_ty == ty->return_ty && params == ty->params)))
    return ty;

  bool was = cc_fn_types(false);
  Type *t = calloc(1, sizeof(Type));
  *t = *ty;
  t->base = base;
  t->origin = origin;
  if (ty->kind == TY_FUNC) {
    t->return_ty = ret_ty;
    t->params = params;
  } else if (ty->kind == TY_VLA) {
    t->vla_len = NULL;
    t->vla_size = NULL;
  } else if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && in_fn) {
    // A copy made for a flexible array member's initializer; its members are its own.
    Member head = {}, *cur = &head;
    for (Member *m = ty->members; m; m = m->next) {
      cur = cur->next = calloc(1, sizeof(Member));
      *cur = *m;
      cur->ty = keep_type(m->ty);
    }
    t->members = head.next;
  }
  cc_fn_types(was);
  return t;
}

// Pointer types are never changed once made (a declarator's name on one is read at once), so
// pointer_to() hands out one per base where it can: the headers alone ask for hundreds. Only types
// that last the whole compile go in, as a function arena's addresses come back for other types.
#define PTR_CACHE 64
static Type *ptr_cache[PTR_CACHE];

void type_reset(void) { memset(ptr_cache, 0, sizeof ptr_cache); }

Type *pointer_to(Type *base) {
  Type **slot = &ptr_cache[((uintptr_t)base >> 3) % PTR_CACHE];
  if (*slot && (*slot)->base == base)
    return *slot;
  Type *ty = new_type(TY_PTR, 4, 4);
  ty->base = base;
  ty->is_unsigned = true;
  if (!cc_in_fn_arena(ty) && !cc_in_fn_arena(base))
    *slot = ty;
  return ty;
}

Type *func_type(Type *return_ty) {
  // The C spec disallows sizeof(<function type>), but
  // GCC allows that and the expression is evaluated to 1.
  Type *ty = new_type(TY_FUNC, 1, 1);
  ty->return_ty = return_ty;
  return ty;
}

Type *array_of(Type *base, int len) {
  Type *ty = new_type(TY_ARRAY, base->size * len, base->align);
  ty->base = base;
  ty->array_len = len;
  return ty;
}

Type *vla_of(Type *base, Node *len) {
  Type *ty = new_type(TY_VLA, 4, 4);
  ty->base = base;
  ty->vla_len = len;
  return ty;
}

Type *enum_type(void) {
  return new_type(TY_ENUM, 4, 4);
}

Type *struct_type(void) {
  return new_type(TY_STRUCT, 0, 1);
}

static Type *get_common_type(Type *ty1, Type *ty2) {
  if (ty1->base)
    return pointer_to(ty1->base);

  if (ty1->kind == TY_FUNC)
    return pointer_to(ty1);
  if (ty2->kind == TY_FUNC)
    return pointer_to(ty2);

  if (ty1->kind == TY_LDOUBLE || ty2->kind == TY_LDOUBLE)
    return ty_ldouble;
  if (ty1->kind == TY_DOUBLE || ty2->kind == TY_DOUBLE)
    return ty_double;
  if (ty1->kind == TY_FLOAT || ty2->kind == TY_FLOAT)
    return ty_float;

  if (ty1->size < 4)
    ty1 = ty_int;
  if (ty2->size < 4)
    ty2 = ty_int;

  if (ty1->size != ty2->size)
    return (ty1->size < ty2->size) ? ty2 : ty1;

  if (ty2->is_unsigned)
    return ty2;
  return ty1;
}

// For many binary operators, we implicitly promote operands so that
// both operands have the same type. Any integral type smaller than
// int is always promoted to int. If the type of one operand is larger
// than the other's (e.g. "long" vs. "int"), the smaller operand will
// be promoted to match with the other.
//
// This operation is called the "usual arithmetic conversion".
static void usual_arith_conv(Node **lhs, Node **rhs) {
  Type *ty = get_common_type((*lhs)->ty, (*rhs)->ty);
  *lhs = implicit_cast(*lhs, ty);
  *rhs = implicit_cast(*rhs, ty);
}

void add_type(Node *node) {
  if (!node || node->ty)
    return;

  add_type(node->lhs);
  add_type(node->rhs);
  if (is_ctl_node(node)) {
    add_type(node->cond);
    add_type(node->then);
    add_type(node->els);
    if (node->kind == ND_FOR) {
      add_type(node->init);
      add_type(node->inc);
    }
  } else if (node->kind == ND_BLOCK || node->kind == ND_STMT_EXPR) {
    for (Node *n = node->body; n; n = n->next)
      add_type(n);
  } else if (node->kind == ND_FUNCALL) {
    for (Node *n = node->args; n; n = n->next)
      add_type(n);
  }

  switch (node->kind) {
  case ND_NUM:
    node->ty = ty_int;
    return;
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
    usual_arith_conv(&node->lhs, &node->rhs);
    node->ty = node->lhs->ty;
    return;
  case ND_NEG: {
    Type *ty = get_common_type(ty_int, node->lhs->ty);
    node->lhs = implicit_cast(node->lhs, ty);
    node->ty = ty;
    return;
  }
  case ND_ASSIGN:
    if (node->lhs->ty->kind == TY_ARRAY)
      error_tok(node->lhs->tok, "not an lvalue");
    if (node->lhs->ty->kind != TY_STRUCT)
      node->rhs = implicit_cast(node->rhs, node->lhs->ty);
    node->ty = node->lhs->ty;
    return;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
    usual_arith_conv(&node->lhs, &node->rhs);
    node->ty = ty_int;
    return;
  case ND_FUNCALL:
    node->ty = node->func_ty->return_ty;
    return;
  case ND_NOT:
  case ND_LOGOR:
  case ND_LOGAND:
    node->ty = ty_int;
    return;
  case ND_BITNOT:
  case ND_SHL:
  case ND_SHR:
    node->ty = node->lhs->ty;
    return;
  case ND_VAR:
  case ND_VLA_PTR:
    node->ty = node->var->ty;
    return;
  case ND_COND:
    if (node->then->ty->kind == TY_VOID || node->els->ty->kind == TY_VOID) {
      node->ty = ty_void;
    } else {
      usual_arith_conv(&node->then, &node->els);
      node->ty = node->then->ty;
    }
    return;
  case ND_COMMA:
    node->ty = node->rhs->ty;
    return;
  case ND_MEMBER:
    node->ty = node->member->ty;
    return;
  case ND_ADDR: {
    Type *ty = node->lhs->ty;
    if (ty->kind == TY_ARRAY)
      node->ty = pointer_to(ty->base);
    else
      node->ty = pointer_to(ty);
    return;
  }
  case ND_DEREF:
    if (!node->lhs->ty->base)
      error_tok(node->tok, "invalid pointer dereference");
    if (node->lhs->ty->base->kind == TY_VOID)
      error_tok(node->tok, "dereferencing a void pointer");

    node->ty = node->lhs->ty->base;
    return;
  case ND_STMT_EXPR:
    if (node->body) {
      Node *stmt = node->body;
      while (stmt->next)
        stmt = stmt->next;
      if (stmt->kind == ND_EXPR_STMT) {
        node->ty = stmt->lhs->ty;
        return;
      }
    }
    error_tok(node->tok, "statement expression returning void is not supported");
    return;
  case ND_LABEL_VAL:
    node->ty = pointer_to(ty_void);
    return;
  case ND_CAS:
    add_type(node->cas_addr);
    add_type(node->cas_old);
    add_type(node->cas_new);
    node->ty = ty_bool;

    if (node->cas_addr->ty->kind != TY_PTR)
      error_tok(node->cas_addr->tok, "pointer expected");
    if (node->cas_old->ty->kind != TY_PTR)
      error_tok(node->cas_old->tok, "pointer expected");
    return;
  case ND_EXCH:
    if (node->lhs->ty->kind != TY_PTR)
      error_tok(node->cas_addr->tok, "pointer expected");
    node->ty = node->lhs->ty->base;
    return;
  }
}
