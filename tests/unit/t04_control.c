/* t04: if/else chains, loops, break/continue, switch (dense, sparse, fallthrough), goto. */
#include <stdio.h>

static const char *classify(int n)
{
    if (n < 0) return "neg";
    else if (n == 0) return "zero";
    else if (n < 10) return "small";
    else return "big";
}

static int dense(int v)       /* likely a jump table */
{
    switch (v) {
    case 0: return 100;
    case 1: return 101;
    case 2: return 102;
    case 3: return 103;
    case 4: return 104;
    case 5: return 105;
    case 6: return 106;
    case 7: return 107;
    default: return -1;
    }
}

static int sparse(int v)      /* compare chain */
{
    switch (v) {
    case -1000: return 1;
    case 7: return 2;
    case 4096: return 3;
    case 0x7fffffff: return 4;
    }
    return 0;
}

static int fall(int v)
{
    int r = 0;
    switch (v) {
    case 1: r += 1;
    case 2: r += 10;
    case 3: r += 100; break;
    case 4: { int k = 5; r = k * 2; } break;
    default: r = -1;
    }
    return r;
}

int main(void)
{
    printf("%s %s %s %s\n", classify(-5), classify(0), classify(3), classify(42));

    int s = 0;
    for (int i = 0; i < 10; i++) s += i;
    printf("for %d\n", s);

    int i = 0, w = 0;
    while (i < 100) { i += 7; w++; }
    printf("while %d %d\n", i, w);

    int d = 0;
    do { d++; } while (d < 0);
    printf("do %d\n", d);

    s = 0;
    for (int j = 0; j < 20; j++) {
        if (j % 2) continue;
        if (j > 12) break;
        s += j;
    }
    printf("brk %d\n", s);

    /* nested loops with break only leaving the inner loop */
    int pairs = 0;
    for (int a = 0; a < 5; a++)
        for (int b = 0; b < 5; b++) {
            if (b > a) break;
            pairs++;
        }
    printf("nested %d\n", pairs);

    printf("dense");
    for (int v = -1; v <= 8; v++) printf(" %d", dense(v));
    printf("\n");
    printf("sparse %d %d %d %d %d\n", sparse(-1000), sparse(7), sparse(4096), sparse(0x7fffffff), sparse(8));
    printf("fall %d %d %d %d %d\n", fall(1), fall(2), fall(3), fall(4), fall(9));

    int g = 0;
again:
    g++;
    if (g < 5) goto again;
    printf("goto %d\n", g);

    /* infinite for with break, comma operator */
    int c1, c2;
    for (c1 = 0, c2 = 10;; c1++, c2--)
        if (c1 >= c2) break;
    printf("comma %d %d\n", c1, c2);
    return 0;
}
