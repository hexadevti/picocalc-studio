/* bench - compiler speed benchmarks. Each one prints a checksum (must not change between compiler
 * versions) and, on the device, its time in microseconds.
 *   bench            run them all
 *   bench <name>     run one (tools/host/bench.py does this under rvsim and counts instructions)
 * Everything is computed by the program's own code: no libc helpers in the timed loops, so the
 * numbers measure the compiler, not the firmware. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#if defined(__PICOCALC__) && !defined(BENCH_SIM)
#include <picocalc.h>
#define NOW() sys_micros()
#else
#define NOW() 0
#endif

/* ---- fib: calls, recursion ---- */
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static uint32_t b_fib(void) { return (uint32_t)fib(27); }

/* ---- sieve: byte arrays, nested loops ---- */
#define SIEVE_N 200000
static uint8_t sieve_buf[SIEVE_N + 1];

static uint32_t b_sieve(void)
{
    uint32_t count = 0;
    for (int pass = 0; pass < 4; pass++) {
        for (int i = 0; i <= SIEVE_N; i++) sieve_buf[i] = 1;
        sieve_buf[0] = sieve_buf[1] = 0;
        for (int i = 2; i * i <= SIEVE_N; i++)
            if (sieve_buf[i])
                for (int j = i * i; j <= SIEVE_N; j += i) sieve_buf[j] = 0;
        count = 0;
        for (int i = 0; i <= SIEVE_N; i++) count += sieve_buf[i];
    }
    return count;
}

/* ---- crc32: shifts, xor, unsigned arithmetic ---- */
#define CRC_LEN 32768
static uint8_t crc_data[CRC_LEN];

static uint32_t crc32(const uint8_t *p, int n)
{
    uint32_t crc = 0xffffffffu;
    for (int i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static uint32_t b_crc(void)
{
    uint32_t x = 12345;
    for (int i = 0; i < CRC_LEN; i++) {
        x = x * 1103515245u + 12345u;
        crc_data[i] = (uint8_t)(x >> 16);
    }
    uint32_t c = 0;
    for (int pass = 0; pass < 2; pass++) c ^= crc32(crc_data, CRC_LEN) + (uint32_t)pass;
    return c;
}

/* ---- mandel: float FPU loops ---- */
static uint32_t b_mandel(void)
{
    uint32_t sum = 0;
    for (int py = 0; py < 100; py++) {
        float ci = -1.2f + (float)py * 0.024f;
        for (int px = 0; px < 160; px++) {
            float cr = -2.1f + (float)px * 0.019f, zr = 0, zi = 0;
            int it = 0;
            while (it < 64 && zr * zr + zi * zi < 4.0f) {
                float t = zr * zr - zi * zi + cr;
                zi = 2.0f * zr * zi + ci;
                zr = t;
                it++;
            }
            sum = sum * 31u + (uint32_t)it;
        }
    }
    return sum;
}

/* ---- sort: array indexing, pointer swaps; plus a linked list ---- */
#define SORT_N 20000
static int sort_a[SORT_N];

static void quicksort(int *a, int lo, int hi)
{
    while (lo < hi) {
        int p = a[(lo + hi) >> 1], i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) {
                int t = a[i];
                a[i] = a[j];
                a[j] = t;
                i++;
                j--;
            }
        }
        if (j - lo < hi - i) {
            quicksort(a, lo, j);
            lo = i;
        } else {
            quicksort(a, i, hi);
            hi = j;
        }
    }
}

struct Node {
    struct Node *next;
    int val;
};
static struct Node nodes[SORT_N];

static uint32_t b_sort(void)
{
    uint32_t x = 777;
    for (int i = 0; i < SORT_N; i++) {
        x = x * 1664525u + 1013904223u;
        sort_a[i] = (int)(x >> 8) - (1 << 23);
    }
    quicksort(sort_a, 0, SORT_N - 1);
    uint32_t h = 0;
    for (int i = 0; i < SORT_N; i += 97) h = h * 33u + (uint32_t)sort_a[i];
    for (int i = 1; i < SORT_N; i++)
        if (sort_a[i - 1] > sort_a[i]) return 0;

    struct Node *head = NULL;   /* build, reverse, walk */
    for (int i = 0; i < SORT_N; i++) {
        nodes[i].val = sort_a[i] & 0xffff;
        nodes[i].next = head;
        head = &nodes[i];
    }
    for (int rep = 0; rep < 5; rep++) {
        struct Node *prev = NULL;
        while (head) {
            struct Node *n = head->next;
            head->next = prev;
            prev = head;
            head = n;
        }
        head = prev;
        for (struct Node *n = head; n; n = n->next) h += (uint32_t)n->val;
    }
    return h;
}

/* ---- str: char loops, small helper calls ---- */
static int my_strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (int)(p - s);
}

static void my_strcpy(char *d, const char *s)
{
    while ((*d++ = *s++)) {}
}

static uint32_t hash_str(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = h * 33u ^ (uint8_t)*s++;
    return h;
}

static int count_words(const char *s)
{
    int n = 0, in = 0;
    for (; *s; s++) {
        int sp = *s == ' ' || *s == '\n';
        if (!sp && !in) n++;
        in = !sp;
    }
    return n;
}

static uint32_t b_str(void)
{
    static const char *const words[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"};
    static char text[4096], copy[4096];
    uint32_t h = 0;
    for (int rep = 0; rep < 40; rep++) {
        int len = 0;
        for (int i = 0; len < 4000; i++) {
            const char *w = words[(i * 7 + rep) & 7];
            my_strcpy(text + len, w);
            len += my_strlen(w);
            text[len++] = (i % 9 == 8) ? '\n' : ' ';
            text[len] = 0;
        }
        my_strcpy(copy, text);
        h = h * 31u + hash_str(copy) + (uint32_t)count_words(copy) + (uint32_t)my_strlen(copy);
    }
    return h;
}

/* ---- matrix: 2D indexing, multiply-accumulate ---- */
#define MAT_N 40
static int mat_a[MAT_N][MAT_N], mat_b[MAT_N][MAT_N], mat_c[MAT_N][MAT_N];

static uint32_t b_matrix(void)
{
    for (int i = 0; i < MAT_N; i++)
        for (int j = 0; j < MAT_N; j++) {
            mat_a[i][j] = (i * 3 + j) % 17 - 8;
            mat_b[i][j] = (i + j * 5) % 13 - 6;
        }
    for (int rep = 0; rep < 4; rep++)
        for (int i = 0; i < MAT_N; i++)
            for (int j = 0; j < MAT_N; j++) {
                int s = 0;
                for (int k = 0; k < MAT_N; k++) s += mat_a[i][k] * mat_b[k][j];
                mat_c[i][j] = s + rep;
            }
    uint32_t h = 0;
    for (int i = 0; i < MAT_N; i++)
        for (int j = 0; j < MAT_N; j++) h = h * 31u + (uint32_t)mat_c[i][j];
    return h;
}

/* ---- table ---- */
struct Bench {
    const char *name;
    uint32_t (*fn)(void);
};
static const struct Bench benches[] = {
    {"fib", b_fib},     {"sieve", b_sieve}, {"crc", b_crc},       {"mandel", b_mandel},
    {"sort", b_sort},   {"str", b_str},     {"matrix", b_matrix},
};
#define NBENCH (int)(sizeof benches / sizeof benches[0])

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
    unsigned long total = 0;
    int ran = 0;
    if (only && !strcmp(only, "none")) return 0;   /* bench.py: the startup cost alone */
    for (int i = 0; i < NBENCH; i++) {
        if (only && strcmp(only, benches[i].name)) continue;
        unsigned long t0 = (unsigned long)NOW();
        uint32_t sum = benches[i].fn();
        unsigned long us = (unsigned long)NOW() - t0;
        total += us;
        ran++;
        printf("%-7s %08lx %9lu us\n", benches[i].name, (unsigned long)sum, us);
    }
    if (!ran) {
        printf("unknown benchmark %s\n", only);
        return 1;
    }
    if (ran > 1) printf("total            %9lu us\n", total);
    return 0;
}
