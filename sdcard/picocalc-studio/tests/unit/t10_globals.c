/* t10: globals - initialised data, zeroed bss, static locals, const tables in rodata,
 * pointer-initialised globals (need load-time relocations), arrays of strings. */
#include <stdio.h>

int g_init = 1234;
int g_zero;
static int s_hidden = -5;
int g_array[8] = { 1, 1, 2, 3, 5, 8 };   /* tail zero-filled */
const int k_table[4] = { 10, 20, 30, 40 };
const char *const k_names[] = { "alpha", "beta", "gamma" };
char g_msg[] = "mutable";
int *g_ptr = &g_array[3];                /* relocated data pointer */
const int *g_kptr = &k_table[2];         /* pointer into rodata */
static long long g_ll = -9000000000LL;
double g_d = 2.5;

struct Rec { const char *name; int val; int (*fn)(int); };
static int twice(int x) { return 2 * x; }
static int square(int x) { return x * x; }
struct Rec g_recs[] = { { "twice", 3, twice }, { "square", 4, square } };   /* relocated fn ptrs */

static int counter(void)
{
    static int n;        /* zero-init static local */
    static int step = 3; /* initialised static local */
    n += step;
    return n;
}

int main(void)
{
    printf("init %d zero %d hidden %d\n", g_init, g_zero, s_hidden);
    int s = 0;
    for (int i = 0; i < 8; i++) s += g_array[i];
    printf("array sum %d last %d\n", s, g_array[7]);
    printf("table %d %d\n", k_table[1], *g_kptr);
    printf("names %s %s %s\n", k_names[0], k_names[1], k_names[2]);
    g_msg[0] = 'M';
    printf("msg %s\n", g_msg);
    *g_ptr = 77;
    printf("ptr %d %d\n", g_array[3], *g_ptr);
    printf("ll %lld d %.2f\n", g_ll, g_d);
    for (unsigned i = 0; i < sizeof g_recs / sizeof g_recs[0]; i++)
        printf("rec %s %d\n", g_recs[i].name, g_recs[i].fn(g_recs[i].val));

    g_zero = 9;
    g_init++;
    printf("modified %d %d\n", g_zero, g_init);

    counter(); counter();
    printf("static local %d\n", counter());

    /* large zero-initialised array lives in bss, must start zeroed */
    static int big[1000];
    int nz = 0;
    for (int i = 0; i < 1000; i++) nz += big[i] != 0;
    big[999] = 1;
    printf("bss nonzero %d\n", nz);
    return 0;
}
