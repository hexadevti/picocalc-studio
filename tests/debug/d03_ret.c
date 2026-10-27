// Debugger: finish / next out through the return trampoline keep the return value (int, float,
// long long, a struct), from a variadic function too, and bt shows the frames (run_tests.py debug).
#include <stdio.h>
#include <stdarg.h>

typedef struct { int a, b, c; } Tri;

int sum(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}

float half(float x) {
    return x / 2.0f;
}

long long big(int k) {
    return (long long)k << 40;
}

Tri tri(int k) {
    Tri t = {k, k * 2, k * 3};
    return t;
}

int outer(int k) {
    int s = sum(3, k, k, k);
    return s + 1;
}

int main(void) {
    int s = sum(4, 1, 2, 3, 4);
    printf("s=%d\n", s);
    float h = half(5.0f);
    printf("h=%.2f\n", (double)h);
    long long b = big(3);
    printf("b=%lld\n", b);
    Tri t = tri(7);
    printf("t=%d %d %d\n", t.a, t.b, t.c);
    int o = outer(2);
    printf("o=%d\n", o);
    return 0;
}
