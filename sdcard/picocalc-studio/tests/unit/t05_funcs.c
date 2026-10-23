/* t05: calling convention - many args (stack args), recursion, mutual recursion, char/short args. */
#include <stdio.h>

static int sum10(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j)
{
    /* a..h in a0..a7, i and j on the stack */
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j;
}

static long long mix(int a, long long b, int c, long long d, int e, long long f)
{
    /* 64-bit args need aligned register pairs; f spills to the stack */
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6;
}

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static unsigned ack(unsigned m, unsigned n)
{
    if (m == 0) return n + 1;
    if (n == 0) return ack(m - 1, 1);
    return ack(m - 1, ack(m, n - 1));
}

static int is_odd(int n);
static int is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }
static int is_odd(int n) { return n == 0 ? 0 : is_even(n - 1); }

static int narrow(signed char c, unsigned char uc, short s, unsigned short us)
{
    return c + uc + s + us;
}

static signed char ret_char(int v) { return (signed char)v; }
static unsigned short ret_ushort(int v) { return (unsigned short)v; }

static void out_params(int *q, int *r, int a, int b) { *q = a / b; *r = a % b; }

static int depth(int n) { volatile char pad[64]; pad[0] = (char)n; return n == 0 ? pad[0] : 1 + depth(n - 1); }

int main(void)
{
    printf("sum10 %d\n", sum10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    printf("mix %lld\n", mix(1, 10000000000LL, 3, -5LL, 7, 123456789012LL));
    printf("fib %d %d %d\n", fib(0), fib(10), fib(20));
    printf("ack %u %u\n", ack(2, 3), ack(3, 3));
    printf("even %d %d odd %d\n", is_even(10), is_even(7), is_odd(7));
    printf("narrow %d\n", narrow(-1, 255, -1000, 65000));
    printf("ret %d %u\n", ret_char(300), ret_ushort(-1));
    int q, r;
    out_params(&q, &r, 47, 5);
    printf("out %d %d\n", q, r);
    printf("depth %d\n", depth(200));
    return 0;
}
