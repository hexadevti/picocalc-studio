// Debugger: breakpoints and step into / over / out, recursion (run_tests.py debug).
#include <stdio.h>

int sq(int x) {
    int r = x * x;
    return r;
}

int fact(int n) {
    if (n <= 1)
        return 1;
    return n * fact(n - 1);
}

int main(void) {
    int a = 3;
    int b = sq(a);
    printf("b=%d\n", b);
    int f = fact(3);
    printf("f=%d\n", f);
    for (int i = 0; i < 3; i++)
        printf("i=%d\n", i);
    return 0;
}
