/* t03: signed/unsigned comparisons, logical short-circuit, ternary. */
#include <stdio.h>

static int calls;
static int side(int v) { calls++; return v; }

int main(void)
{
    volatile int m1 = -1, z = 0, p1 = 1;
    volatile unsigned u1 = 1u;

    printf("s %d %d %d %d %d %d\n", m1 < p1, m1 > p1, m1 <= m1, p1 >= z, z == z, m1 != p1);
    /* -1 converted to unsigned is huge */
    printf("u %d %d\n", (unsigned)m1 < u1, (unsigned)m1 > u1);
    printf("mixed %d\n", m1 < (int)u1);

    volatile unsigned char uc = 200;
    volatile signed char sc = -56;
    printf("chars %d %d %d\n", uc > 100, sc < 0, uc == (unsigned char)sc);

    calls = 0;
    int r = side(0) && side(1);
    printf("and0 r=%d calls=%d\n", r, calls);
    calls = 0;
    r = side(1) && side(2);
    printf("and1 r=%d calls=%d\n", r, calls);
    calls = 0;
    r = side(1) || side(0);
    printf("or1 r=%d calls=%d\n", r, calls);
    calls = 0;
    r = side(0) || side(0);
    printf("or0 r=%d calls=%d\n", r, calls);

    printf("not %d %d %d\n", !z, !p1, !!m1);

    int t = p1 ? 10 : 20;
    int f = z ? 10 : 20;
    int nested = m1 < 0 ? (p1 > 0 ? 1 : 2) : 3;
    printf("tern %d %d %d\n", t, f, nested);

    /* comparison results are ints usable in arithmetic */
    printf("sum %d\n", (m1 < z) + (z < p1) + (p1 < m1));

    long long big = 0x100000000LL;
    printf("ll %d %d\n", big > 0x7fffffff, (long long)m1 < big);
    return 0;
}
