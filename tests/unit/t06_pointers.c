/* t06: pointer arithmetic, pointer-to-pointer, arrays vs pointers, 2-D arrays, void*. */
#include <stdio.h>
#include <stddef.h>

static void bump_all(int *p, int n) { for (int i = 0; i < n; i++) p[i] += 100; }
static void set_ptr(int **pp, int *target) { *pp = target; }
static int sum2d(int rows, int m[][4]) { int s = 0; for (int r = 0; r < rows; r++) for (int c = 0; c < 4; c++) s += m[r][c]; return s; }

int main(void)
{
    int a[5] = { 1, 2, 3, 4, 5 };
    int *p = a;
    printf("idx %d %d %d\n", *p, *(p + 2), p[4]);
    p += 3;
    printf("move %d %d\n", *p, p[-1]);
    printf("diff %d\n", (int)(p - a));
    printf("cmp %d %d\n", p > a, p == &a[3]);

    int *end = a + 5, n = 0;
    for (int *q = a; q != end; q++) n += *q;
    printf("walk %d\n", n);

    bump_all(a, 5);
    printf("bumped %d %d\n", a[0], a[4]);

    int x = 1, y = 2, *px = &x;
    int **ppx = &px;
    **ppx = 10;
    set_ptr(ppx, &y);
    *px = 20;
    printf("pp %d %d\n", x, y);

    int m[3][4];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) m[r][c] = r * 10 + c;
    printf("2d %d %d %d\n", m[2][3], *(*(m + 1) + 2), sum2d(3, m));
    printf("rowsize %d %d\n", (int)sizeof m, (int)sizeof m[0]);
    int (*row)[4] = m + 1;
    printf("rowptr %d\n", (*row)[3]);

    char buf[8] = "abcdefg";
    char *cp = buf;
    short sh[4] = { 1, 2, 3, 4 };
    short *sp = sh;
    printf("stride %d %d\n", (int)((char *)(sp + 3) - (char *)sp), (int)((cp + 3) - cp));

    void *vp = a;
    int *back = (int *)vp;
    printf("void %d\n", back[1]);

    const char *strs[] = { "zero", "one", "two" };
    const char **sp2 = strs;
    printf("strs %s %c\n", sp2[2], *sp2[1]);

    int *null = NULL;
    printf("null %d\n", null == 0);

    /* pointer to local in loop, increment through struct-less byte view */
    unsigned int word = 0x11223344u;
    unsigned char *bytes = (unsigned char *)&word;
    printf("endian %02x %02x\n", bytes[0], bytes[3]);
    return 0;
}
