/* t19: large stack frames - locals beyond the 12-bit signed immediate range (+-2047) need
 * lui/add address materialisation; also big arrays passed down, alloca-free VLAs. */
#include <stdio.h>
#include <string.h>

static int big_frame(int seed)
{
    int before = seed;            /* near the frame base */
    char filler[5000];            /* pushes later locals past +-2047 */
    int after = seed * 2;
    double d = 0.5;
    long long ll = 1;
    memset(filler, 0, sizeof filler);
    for (int i = 0; i < 5000; i++) filler[i] = (char)(i * seed);
    unsigned sum = 0;
    for (int i = 0; i < 5000; i++) sum += (unsigned char)filler[i];
    ll <<= 40;
    d *= after;
    return (int)(sum + (unsigned)before + (unsigned)after + (unsigned)d + (unsigned)(ll >> 38));
}

static long sum_matrix(void)
{
    int m[40][40];                /* 6400 bytes */
    for (int r = 0; r < 40; r++)
        for (int c = 0; c < 40; c++) m[r][c] = r * c - c;
    long s = 0;
    for (int r = 0; r < 40; r++) s += m[r][39 - r];
    return s + m[39][39];
}

static int vla(int n)
{
    int a[n];                     /* variable length array */
    for (int i = 0; i < n; i++) a[i] = i * i;
    int s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return s + (int)sizeof a;
}

static int nested_big(int depth)
{
    char buf[3000];
    buf[0] = (char)depth;
    buf[2999] = (char)(depth + 1);
    if (depth == 0) return buf[0] + buf[2999];
    return buf[0] + buf[2999] + nested_big(depth - 1);
}

int main(void)
{
    printf("frame %d %d\n", big_frame(3), big_frame(7));
    printf("matrix %ld\n", sum_matrix());
    printf("vla %d %d\n", vla(10), vla(1000));
    printf("nested %d\n", nested_big(3));
    return 0;
}
