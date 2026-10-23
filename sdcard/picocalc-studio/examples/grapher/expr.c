/* expr.c - recursive-descent parser to an AST, evaluator, and symbolic differentiation.
 *
 *   expr   := term (('+' | '-') term)*
 *   term   := unary (('*' | '/') unary | implicit-multiply)*
 *   unary  := '-' unary | power
 *   power  := atom ('^' unary)?            right associative: 2^3^2 = 2^9
 *   atom   := number | 'x' | 't' | 'pi' | 'e' | func '(' expr ')' | '(' expr ')'
 *
 * Implicit multiplication: 2x, 3sin(x), (x+1)(x-1), 2pi.
 */
#include "expr.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const func_names[NFUNC] = {
    "sin", "cos", "tan", "asin", "acos", "atan", "sqrt", "abs", "ln", "log", "exp", "floor", "sinh", "cosh",
};

typedef struct {
    const char *src, *p;
    const char *err;
    int err_pos;
} Parser;

static Node *mk(NodeKind k, Node *a, Node *b)
{
    Node *n = calloc(1, sizeof(Node));
    n->kind = k;
    n->a = a;
    n->b = b;
    return n;
}

static Node *num(float v)
{
    Node *n = mk(N_NUM, NULL, NULL);
    n->val = v;
    return n;
}

static Node *func(FuncId f, Node *a)
{
    Node *n = mk(N_FUNC, a, NULL);
    n->fn = f;
    return n;
}

static void fail(Parser *ps, const char *msg)
{
    if (!ps->err) { ps->err = msg; ps->err_pos = (int)(ps->p - ps->src); }
}

static void skip(Parser *ps) { while (*ps->p == ' ') ps->p++; }

static Node *parse_expr(Parser *ps);
static Node *parse_unary(Parser *ps);

static Node *parse_atom(Parser *ps)
{
    skip(ps);
    const char *s = ps->p;
    if (isdigit((unsigned char)*s) || (*s == '.' && isdigit((unsigned char)s[1]))) {
        char *end;
        float v = strtof(s, &end);
        ps->p = end;
        return num(v);
    }
    if (*s == '(') {
        ps->p++;
        Node *n = parse_expr(ps);
        skip(ps);
        if (*ps->p != ')') { fail(ps, "expected )"); expr_free(n); return NULL; }
        ps->p++;
        return n;
    }
    if (isalpha((unsigned char)*s)) {
        int len = 0;
        while (isalpha((unsigned char)s[len])) len++;
        /* the longest function name the letters start with ("sinh" over "sin"); "xsin" starts
         * with none, so it reads as x times sin */
        int best = -1, best_len = 0;
        for (int f = 0; f < NFUNC; f++) {
            int fl = (int)strlen(func_names[f]);
            if (fl <= len && fl > best_len && !strncmp(s, func_names[f], (size_t)fl)) { best = f; best_len = fl; }
        }
        if (best >= 0) {
            ps->p = s + best_len;
            skip(ps);
            if (*ps->p != '(') { fail(ps, "expected ( after function"); return NULL; }
            Node *arg = parse_atom(ps);   /* parses the parenthesised argument */
            return arg ? func((FuncId)best, arg) : NULL;
        }
        if (len >= 2 && !strncmp(s, "pi", 2)) { ps->p = s + 2; return num(3.14159265f); }
        if (*s == 'x') { ps->p = s + 1; return mk(N_X, NULL, NULL); }
        if (*s == 't') { ps->p = s + 1; return mk(N_T, NULL, NULL); }
        if (*s == 'e') { ps->p = s + 1; return num(2.71828183f); }
        fail(ps, "unknown name");
        return NULL;
    }
    fail(ps, *s ? "unexpected character" : "unexpected end");
    return NULL;
}

static Node *parse_power(Parser *ps)
{
    Node *base = parse_atom(ps);
    if (!base) return NULL;
    skip(ps);
    if (*ps->p == '^') {
        ps->p++;
        Node *e = parse_unary(ps);
        if (!e) { expr_free(base); return NULL; }
        return mk(N_POW, base, e);
    }
    return base;
}

static Node *parse_unary(Parser *ps)
{
    skip(ps);
    if (*ps->p == '-') {
        ps->p++;
        Node *a = parse_unary(ps);
        return a ? mk(N_NEG, a, NULL) : NULL;
    }
    if (*ps->p == '+') { ps->p++; return parse_unary(ps); }
    return parse_power(ps);
}

static Node *parse_term(Parser *ps)
{
    Node *n = parse_unary(ps);
    while (n) {
        skip(ps);
        char c = *ps->p;
        NodeKind k;
        if (c == '*' || c == '/') { ps->p++; k = c == '*' ? N_MUL : N_DIV; }
        else if (c == '(' || isalpha((unsigned char)c) || isdigit((unsigned char)c) || c == '.') k = N_MUL;   /* implicit */
        else break;
        Node *r = parse_unary(ps);
        if (!r) { expr_free(n); return NULL; }
        n = mk(k, n, r);
    }
    return n;
}

static Node *parse_expr(Parser *ps)
{
    Node *n = parse_term(ps);
    while (n) {
        skip(ps);
        char c = *ps->p;
        if (c != '+' && c != '-') break;
        ps->p++;
        Node *r = parse_term(ps);
        if (!r) { expr_free(n); return NULL; }
        n = mk(c == '+' ? N_ADD : N_SUB, n, r);
    }
    return n;
}

Node *expr_parse(const char *src, const char **err_msg, int *err_pos)
{
    Parser ps = { src, src, NULL, 0 };
    Node *n = parse_expr(&ps);
    skip(&ps);
    if (n && *ps.p) { fail(&ps, "unexpected character"); expr_free(n); n = NULL; }
    if (!n && !ps.err) fail(&ps, "empty");
    *err_msg = ps.err;
    *err_pos = ps.err_pos;
    return n;
}

/* ------------------------------------------------------------------ evaluate */

static float apply(FuncId f, float v)
{
    switch (f) {
    case F_SIN: return sinf(v);
    case F_COS: return cosf(v);
    case F_TAN: return tanf(v);
    case F_ASIN: return v < -1 || v > 1 ? NAN : atan2f(v, sqrtf(1 - v * v));
    case F_ACOS: return v < -1 || v > 1 ? NAN : atan2f(sqrtf(1 - v * v), v);
    case F_ATAN: return atan2f(v, 1.0f);
    case F_SQRT: return v < 0 ? NAN : sqrtf(v);
    case F_ABS: return fabsf(v);
    case F_LN: return v <= 0 ? NAN : logf(v);
    case F_LOG: return v <= 0 ? NAN : logf(v) * 0.434294482f;
    case F_EXP: return expf(v);
    case F_FLOOR: return floorf(v);
    case F_SINH: return (expf(v) - expf(-v)) * 0.5f;
    case F_COSH: return (expf(v) + expf(-v)) * 0.5f;
    default: return NAN;
    }
}

float expr_eval(const Node *n, float x, float t)
{
    switch (n->kind) {
    case N_NUM: return n->val;
    case N_X: return x;
    case N_T: return t;
    case N_NEG: return -expr_eval(n->a, x, t);
    case N_ADD: return expr_eval(n->a, x, t) + expr_eval(n->b, x, t);
    case N_SUB: return expr_eval(n->a, x, t) - expr_eval(n->b, x, t);
    case N_MUL: return expr_eval(n->a, x, t) * expr_eval(n->b, x, t);
    case N_DIV: {
        float d = expr_eval(n->b, x, t);
        return d == 0 ? NAN : expr_eval(n->a, x, t) / d;
    }
    case N_POW: {
        float b = expr_eval(n->a, x, t), e = expr_eval(n->b, x, t);
        if (b < 0 && e != floorf(e)) return NAN;
        return powf(b, e);
    }
    case N_FUNC: return apply(n->fn, expr_eval(n->a, x, t));
    }
    return NAN;
}

/* --------------------------------------------------------------- derivative */

static Node *copy(const Node *n)
{
    if (!n) return NULL;
    Node *c = mk(n->kind, copy(n->a), copy(n->b));
    c->fn = n->fn;
    c->val = n->val;
    return c;
}

static bool is_num(const Node *n, float v) { return n->kind == N_NUM && n->val == v; }

/* constructors that fold constants and drop 0 / 1 terms, so derivatives stay readable */
static Node *add(Node *a, Node *b)
{
    if (is_num(a, 0)) { expr_free(a); return b; }
    if (is_num(b, 0)) { expr_free(b); return a; }
    if (a->kind == N_NUM && b->kind == N_NUM) { float v = a->val + b->val; expr_free(a); expr_free(b); return num(v); }
    return mk(N_ADD, a, b);
}

static Node *neg(Node *a)
{
    if (a->kind == N_NUM) { a->val = -a->val; return a; }
    if (a->kind == N_NEG) { Node *r = a->a; a->a = NULL; expr_free(a); return r; }
    return mk(N_NEG, a, NULL);
}

static Node *sub(Node *a, Node *b)
{
    if (is_num(b, 0)) { expr_free(b); return a; }
    if (is_num(a, 0)) { expr_free(a); return neg(b); }
    if (a->kind == N_NUM && b->kind == N_NUM) { float v = a->val - b->val; expr_free(a); expr_free(b); return num(v); }
    return mk(N_SUB, a, b);
}

static Node *mul(Node *a, Node *b)
{
    if (is_num(a, 0) || is_num(b, 0)) { expr_free(a); expr_free(b); return num(0); }
    if (is_num(a, 1)) { expr_free(a); return b; }
    if (is_num(b, 1)) { expr_free(b); return a; }
    if (a->kind == N_NUM && b->kind == N_NUM) { float v = a->val * b->val; expr_free(a); expr_free(b); return num(v); }
    if (b->kind == N_NUM) { Node *t = a; a = b; b = t; }   /* constants first: 2*cos(x) */
    return mk(N_MUL, a, b);
}

static Node *dvd(Node *a, Node *b)
{
    if (is_num(a, 0)) { expr_free(b); return a; }
    if (is_num(b, 1)) { expr_free(b); return a; }
    return mk(N_DIV, a, b);
}

static Node *pw(Node *a, Node *b)
{
    if (is_num(b, 1)) { expr_free(b); return a; }
    if (is_num(b, 0)) { expr_free(a); expr_free(b); return num(1); }
    return mk(N_POW, a, b);
}

static bool has_x(const Node *n)
{
    if (!n) return false;
    return n->kind == N_X || has_x(n->a) || has_x(n->b);
}

Node *expr_deriv(const Node *n)
{
    if (!n) return NULL;
    if (!has_x(n)) return num(0);
    const Node *u = n->a, *v = n->b;
    switch (n->kind) {
    case N_X: return num(1);
    case N_NEG: return neg(expr_deriv(u));
    case N_ADD: return add(expr_deriv(u), expr_deriv(v));
    case N_SUB: return sub(expr_deriv(u), expr_deriv(v));
    case N_MUL: return add(mul(expr_deriv(u), copy(v)), mul(copy(u), expr_deriv(v)));
    case N_DIV:
        if (!has_x(v)) return dvd(expr_deriv(u), copy(v));   /* u / c */
        /* (u'v - uv') / v^2 */
        return dvd(sub(mul(expr_deriv(u), copy(v)), mul(copy(u), expr_deriv(v))), pw(copy(v), num(2)));
    case N_POW:
        if (!has_x(v))   /* u^c: c u^(c-1) u' */
            return mul(mul(copy(v), pw(copy(u), sub(copy(v), num(1)))), expr_deriv(u));
        if (u->kind == N_NUM)   /* c^v: c^v ln(c) v' */
            return mul(mul(copy(n), num(logf(u->val))), expr_deriv(v));
        /* general: u^v (v' ln u + v u'/u) */
        return mul(copy(n), add(mul(expr_deriv(v), func(F_LN, copy(u))), dvd(mul(copy(v), expr_deriv(u)), copy(u))));
    case N_FUNC: {
        Node *du = expr_deriv(u), *outer;
        switch (n->fn) {
        case F_SIN: outer = func(F_COS, copy(u)); break;
        case F_COS: outer = neg(func(F_SIN, copy(u))); break;
        case F_TAN: outer = dvd(num(1), pw(func(F_COS, copy(u)), num(2))); break;
        case F_ASIN: outer = dvd(num(1), func(F_SQRT, sub(num(1), pw(copy(u), num(2))))); break;
        case F_ACOS: outer = neg(dvd(num(1), func(F_SQRT, sub(num(1), pw(copy(u), num(2)))))); break;
        case F_ATAN: outer = dvd(num(1), add(num(1), pw(copy(u), num(2)))); break;
        case F_SQRT: outer = dvd(num(0.5f), func(F_SQRT, copy(u))); break;
        case F_ABS: outer = dvd(copy(u), func(F_ABS, copy(u))); break;
        case F_LN: outer = dvd(num(1), copy(u)); break;
        case F_LOG: outer = dvd(num(0.434294482f), copy(u)); break;
        case F_EXP: outer = copy(n); break;
        case F_FLOOR: outer = num(0); break;
        case F_SINH: outer = func(F_COSH, copy(u)); break;
        case F_COSH: outer = func(F_SINH, copy(u)); break;
        default: outer = num(0);
        }
        return mul(outer, du);
    }
    default: return num(0);
    }
}

/* ------------------------------------------------------------------ helpers */

bool expr_uses_t(const Node *n)
{
    if (!n) return false;
    return n->kind == N_T || expr_uses_t(n->a) || expr_uses_t(n->b);
}

int expr_count(const Node *n) { return n ? 1 + expr_count(n->a) + expr_count(n->b) : 0; }

void expr_free(Node *n)
{
    if (!n) return;
    expr_free(n->a);
    expr_free(n->b);
    free(n);
}

static int prec(const Node *n)
{
    switch (n->kind) {
    case N_ADD: case N_SUB: return 1;
    case N_MUL: case N_DIV: return 2;
    case N_NEG: return 3;
    case N_POW: return 4;
    default: return 5;
    }
}

static void put(char **p, char *end, const char *s)
{
    while (*s && *p < end - 1) *(*p)++ = *s++;
    **p = 0;
}

static void print_rec(const Node *n, char **p, char *end, int parent_prec, bool right)
{
    int pr = prec(n);
    bool paren = pr < parent_prec || (right && pr == parent_prec && pr != 4);
    if (paren) put(p, end, "(");
    char tmp[24];
    switch (n->kind) {
    case N_NUM:
        if (n->val == 3.14159265f) put(p, end, "pi");
        else if (n->val == 2.71828183f) put(p, end, "e");
        else { snprintf(tmp, sizeof tmp, "%g", (double)n->val); put(p, end, tmp); }
        break;
    case N_X: put(p, end, "x"); break;
    case N_T: put(p, end, "t"); break;
    case N_NEG: put(p, end, "-"); print_rec(n->a, p, end, 3, false); break;
    case N_FUNC:
        put(p, end, func_names[n->fn]);
        put(p, end, "(");
        print_rec(n->a, p, end, 0, false);
        put(p, end, ")");
        break;
    default: {
        static const char *const ops[] = { [N_ADD] = "+", [N_SUB] = "-", [N_MUL] = "*", [N_DIV] = "/", [N_POW] = "^" };
        print_rec(n->a, p, end, pr == 4 ? 5 : pr, false);
        put(p, end, ops[n->kind]);
        print_rec(n->b, p, end, pr, true);
    }
    }
    if (paren) put(p, end, ")");
}

char *expr_print(const Node *n, char *buf, int size)
{
    char *p = buf;
    buf[0] = 0;
    if (n) print_rec(n, &p, buf + size, 0, false);
    return buf;
}
