/* expr.h - expression parser, evaluator and symbolic derivative for the grapher */
#ifndef EXPR_H
#define EXPR_H

#include <stdbool.h>

typedef enum {
    N_NUM, N_X, N_T,                  /* leaves: constant, the variable x, the time t */
    N_NEG, N_ADD, N_SUB, N_MUL, N_DIV, N_POW,
    N_FUNC,
} NodeKind;

typedef enum {
    F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN, F_SQRT, F_ABS, F_LN, F_LOG, F_EXP, F_FLOOR, F_SINH, F_COSH,
    NFUNC
} FuncId;

typedef struct Node Node;
struct Node {
    NodeKind kind;
    FuncId fn;
    float val;
    Node *a, *b;
};

/* Parse src. Returns NULL on error, with *err_msg and *err_pos (0-based column) set. */
Node *expr_parse(const char *src, const char **err_msg, int *err_pos);
float expr_eval(const Node *n, float x, float t);
Node *expr_deriv(const Node *n);       /* d/dx, simplified; NULL if n is NULL */
bool expr_uses_t(const Node *n);
int expr_count(const Node *n);         /* number of nodes */
void expr_free(Node *n);
/* Prints n into buf (fully parenthesised where needed); returns buf. */
char *expr_print(const Node *n, char *buf, int size);

#endif
