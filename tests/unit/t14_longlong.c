/* t14: 64-bit integers on RV32 - register pairs, carries, libgcc helpers (__divdi3, __udivdi3,
 * __moddi3, __umoddi3, __ashldi3, __lshrdi3, __ashrdi3), comparisons across the halves. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

static uint64_t fact(int n) { return n <= 1 ? 1u : (uint64_t)n * fact(n - 1); }
static int64_t sum_ll(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e) { return a + b + c + d + e; }

int main(void)
{
    volatile int64_t a = 0x00000001FFFFFFFFLL, b = 1;
    printf("carry %" PRId64 " %" PRIx64 "\n", a + b, (uint64_t)(a + b));
    volatile int64_t m = -1;
    printf("borrow %" PRId64 "\n", (int64_t)0x100000000LL - b);
    printf("neg %" PRId64 " %" PRId64 "\n", -a, m * a);

    volatile int64_t x = 1234567890123LL, y = -987654321LL;
    printf("mul %" PRId64 "\n", x * 3);
    printf("div %" PRId64 " %" PRId64 " %" PRId64 "\n", x / 1000, x / y, y / 7);
    printf("mod %" PRId64 " %" PRId64 "\n", x % 1000, y % 7);
    volatile uint64_t ux = 0xFEDCBA9876543210ULL;
    printf("udiv %" PRIu64 " %" PRIu64 "\n", ux / 10, ux % 10);

    volatile int sh = 33;
    printf("shl %" PRIx64 "\n", (uint64_t)1 << sh);
    printf("shr %" PRIx64 " sar %" PRId64 "\n", ux >> sh, (int64_t)-0x100000000LL >> 4);
    printf("bits %" PRIx64 " %" PRIx64 " %" PRIx64 "\n", ux & 0xFFFF0000FFFFULL, ux | 1, ux ^ ~0ULL);

    printf("cmp %d %d %d %d\n", x > y, (uint64_t)y > (uint64_t)x, a == 0x1FFFFFFFFLL, (int64_t)0x100000000LL > 0x7FFFFFFF);

    printf("fact20 %" PRIu64 "\n", fact(20));
    printf("sum %" PRId64 "\n", sum_ll(1, 2, 3, 4, 0x7FFFFFFFFFLL));

    int32_t small = -5;
    int64_t widened = small;
    uint32_t us = 0xFFFFFFFFu;
    uint64_t uwidened = us;
    printf("widen %" PRId64 " %" PRIu64 " trunc %d\n", widened, uwidened, (int)(int64_t)0x123456789LL);

    unsigned long long acc = 0;
    for (int i = 0; i < 64; i++) acc = acc * 3 + (unsigned)i;
    printf("acc %llu\n", acc);
    return 0;
}
