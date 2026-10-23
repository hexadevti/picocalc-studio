/* t21: constant expressions in static initializers - negative int constants converted to float,
 * double and long long (a 32-bit result must be sign-extended, not zero-extended, before the
 * conversion), mixed int/float folding, and unsigned 32-bit values that must stay positive. */
#include <stdio.h>

static const float fa[4] = { 11, -7, -4, -(7) };
static float f1 = -7;
float f2 = -2147483647 - 1;
static double d1 = -7;
static double d2 = -3 * 5 + 1;
static long long ll1 = -7;
static long long ll2 = -1 - 0x7fffffff;
static long long ll3 = (int)0xfffffff9u;
static long long ll4 = 0xfffffff9u;
static long long ll5 = -7.5f;
static unsigned long long ull = -1;
static float fu = 4294967295u;
static float fmix = -3 + 0.5f;
static double dc = (short)-2 * 3;

int main(void)
{
    static float local = -7;
    printf("fa %g %g %g %g\n", (double)fa[0], (double)fa[1], (double)fa[2], (double)fa[3]);
    printf("f %g %g local %g\n", (double)f1, (double)f2, (double)local);
    printf("d %g %g\n", d1, d2);
    printf("ll %lld %lld %lld %lld %lld\n", ll1, ll2, ll3, ll4, ll5);
    printf("ull %llu\n", ull);
    printf("fu %g fmix %g dc %g\n", (double)fu, (double)fmix, dc);
    return 0;
}
