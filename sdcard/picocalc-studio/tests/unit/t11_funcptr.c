/* t11: function pointers - tables, callbacks, firmware calling back into the module (qsort, bsearch). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*op_fn)(int, int);
static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
static int op_mul(int a, int b) { return a * b; }
static int op_max(int a, int b) { return a > b ? a : b; }

static const struct { char sym; op_fn fn; } ops[] = {
    { '+', op_add }, { '-', op_sub }, { '*', op_mul }, { 'M', op_max },
};

static int apply(op_fn f, int a, int b) { return f(a, b); }
static op_fn pick(char sym)
{
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++)
        if (ops[i].sym == sym) return ops[i].fn;
    return NULL;
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static int fold(const int *v, int n, int init, int (*f)(int, int))
{
    for (int i = 0; i < n; i++) init = f(init, v[i]);
    return init;
}

int main(void)
{
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++)
        printf("%c=%d ", ops[i].sym, apply(ops[i].fn, 12, 5));
    printf("\n");
    printf("pick %d null %d\n", pick('*')(6, 7), pick('?') == NULL);

    int v[] = { 42, -3, 17, 99, 0, 8, -50, 23 };
    int n = (int)(sizeof v / sizeof v[0]);
    qsort(v, (size_t)n, sizeof v[0], cmp_int);
    printf("sorted");
    for (int i = 0; i < n; i++) printf(" %d", v[i]);
    printf("\n");

    int key = 17;
    int *hit = bsearch(&key, v, (size_t)n, sizeof v[0], cmp_int);
    printf("bsearch %d idx %d\n", hit ? *hit : -1, hit ? (int)(hit - v) : -1);

    const char *words[] = { "pear", "apple", "fig", "banana" };
    qsort(words, 4, sizeof words[0], cmp_str);
    printf("words %s %s %s %s\n", words[0], words[1], words[2], words[3]);

    printf("fold %d %d\n", fold(v, n, 0, op_add), fold(v, n, -1000, op_max));

    int (*arr[2])(int, int) = { op_sub, op_mul };
    int (*(*parr))(int, int) = arr;
    printf("arrfp %d %d\n", arr[0](9, 4), parr[1](9, 4));
    return 0;
}
