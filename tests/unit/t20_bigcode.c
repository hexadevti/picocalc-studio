/* t20: large code - conditional branches whose targets are beyond +-4 KiB (B-type range) must be
 * relaxed into an inverted branch + jal; jal beyond +-1 MiB is not expected in one module.
 * Also a 256-case switch and many small functions. */
#include <stdio.h>

#define STEP(i)  acc = acc * 31u + (unsigned)(i) + (acc >> 7);
#define STEP4(i)  STEP(i) STEP((i) + 1) STEP((i) + 2) STEP((i) + 3)
#define STEP16(i) STEP4(i) STEP4((i) + 4) STEP4((i) + 8) STEP4((i) + 12)
#define STEP64(i) STEP16(i) STEP16((i) + 16) STEP16((i) + 32) STEP16((i) + 48)
#define STEP256(i) STEP64(i) STEP64((i) + 64) STEP64((i) + 128) STEP64((i) + 192)

static unsigned long_body(int n)
{
    unsigned acc = 1;
    for (int k = 0; k < n; k++) {      /* loop back-edge spans the whole body (> 4 KiB) */
        if (acc == 0xDEADBEEFu) break; /* forward branch over the body */
        STEP256(k)
        STEP256(k + 1000)
    }
    return acc;
}

#define C(n) case n: return (n) * 3 + 1;
#define C4(n)  C(n) C((n) + 1) C((n) + 2) C((n) + 3)
#define C16(n) C4(n) C4((n) + 4) C4((n) + 8) C4((n) + 12)
#define C64(n) C16(n) C16((n) + 16) C16((n) + 32) C16((n) + 48)

static int big_switch(int v)
{
    switch (v) {
    C64(0) C64(64) C64(128) C64(192)
    default: return -1;
    }
}

#define F(n) static int f##n(int x) { return x * (n) + 1; }
F(0) F(1) F(2) F(3) F(4) F(5) F(6) F(7) F(8) F(9)
F(10) F(11) F(12) F(13) F(14) F(15)
static int (*const ftab[16])(int) = { f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15 };

int main(void)
{
    printf("long %u %u\n", long_body(1), long_body(10));
    int s = 0;
    for (int v = -1; v <= 256; v++) s += big_switch(v);
    printf("switch %d %d %d\n", big_switch(0), big_switch(255), s);
    int t = 0;
    for (int i = 0; i < 16; i++) t += ftab[i](i);
    printf("ftab %d\n", t);
    return 0;
}
