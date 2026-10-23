/* t02: integer arithmetic, unsigned div/rem, wraparound, shifts, bitwise ops. */
#include <stdio.h>
#include <limits.h>

/* volatile stops gcc folding everything, so the reference build exercises real code too */
static volatile int vi[] = { 7, -7, 3, -3, 100, 0x7fffffff, 1 };
static volatile unsigned vu[] = { 7u, 3u, 0xffffffffu, 0x80000000u, 10u };

int main(void)
{
    int a = vi[0], b = vi[2], na = vi[1], nb = vi[3];
    printf("add %d %d %d\n", a + b, a + nb, na + nb);
    printf("sub %d %d %d\n", a - b, b - a, na - nb);
    printf("mul %d %d %d\n", a * b, a * nb, na * nb);
    printf("div %d %d %d %d\n", a / b, na / b, a / nb, na / nb);
    printf("rem %d %d %d %d\n", a % b, na % b, a % nb, na % nb);

    unsigned ua = vu[0], ub = vu[1], umax = vu[2], uhi = vu[3];
    printf("udiv %u %u %u\n", ua / ub, umax / ub, uhi / vu[4]);
    printf("urem %u %u %u\n", ua % ub, umax % ub, uhi % vu[4]);

    int imax = vi[5];
    printf("wrap %u %u\n", (unsigned)imax + 1u, umax + 1u);
    printf("neg %d %d\n", -a, -na);

    int one = vi[6];
    printf("shl %d %d %u\n", one << 4, one << 30, (unsigned)one << 31);
    printf("sar %d %d %d\n", na >> 1, -256 >> vi[2], vi[4] >> 2);
    printf("shr %u %u\n", umax >> 4, uhi >> 31);

    printf("and %d or %d xor %d not %d\n", a & b, a | 8, a ^ b, ~a);
    printf("hex %x %X %08x\n", 0xdeadbeefu, 255u, (unsigned)vi[4]);

    int x = 10;
    x += 5; printf("x=%d ", x);
    x -= 3; printf("x=%d ", x);
    x *= 4; printf("x=%d ", x);
    x /= 5; printf("x=%d ", x);
    x %= 4; printf("x=%d ", x);
    x <<= 3; printf("x=%d ", x);
    x >>= 1; printf("x=%d ", x);
    x |= 1; printf("x=%d ", x);
    x &= 6; printf("x=%d ", x);
    x ^= 15; printf("x=%d\n", x);

    int i = 5;
    int pre = ++i, post = i++;
    printf("incdec %d %d %d ", pre, post, i);
    pre = --i; post = i--;
    printf("%d %d %d\n", pre, post, i);

    printf("limits %d %d %u\n", INT_MAX, INT_MIN, UINT_MAX);

    /* constant-folded expressions must agree with runtime ones */
    printf("fold %d %d %u\n", 7 * 3 - 4 / 2, (1 << 10) | 3, 0xffffffffu / 3u);
    return 0;
}
