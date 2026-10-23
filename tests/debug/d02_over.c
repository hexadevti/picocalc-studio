// Debugger: next over recursion, a breakpoint hit while stepping, breakpoints on lines without
// code, globals and floats across stops (run_tests.py debug).
#include <stdio.h>

int calls;
float acc = 0.5f;

int fib(int n) {
    calls++;
    if (n < 2) {
        acc = acc * 1.5f;

        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

int main(void) {
    int x = fib(4);
    printf("x=%d calls=%d\n", x, calls);
    double d = 0.25;
    for (int i = 0; i < 2; i++) { d = d * 3.0; }
    printf("d=%.4f acc=%.4f\n", d, (double)acc);
    return 0;
}
