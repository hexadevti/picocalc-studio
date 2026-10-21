/* t15: variadic functions - calling firmware printf with mixed types (doubles go in aligned integer
 * register pairs under ilp32f), and user-defined variadics with va_list / va_copy / vsnprintf. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static int sum_ints(int count, ...)
{
    va_list ap;
    va_start(ap, count);
    int s = 0;
    for (int i = 0; i < count; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}

static double avg(int count, ...)
{
    va_list ap;
    va_start(ap, count);
    double s = 0;
    for (int i = 0; i < count; i++) s += va_arg(ap, double);
    va_end(ap);
    return count ? s / count : 0.0;
}

static void typed(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        switch (*p) {
        case 'i': printf("i:%d ", va_arg(ap, int)); break;
        case 'l': printf("l:%lld ", va_arg(ap, long long)); break;
        case 'd': printf("d:%.2f ", va_arg(ap, double)); break;
        case 's': printf("s:%s ", va_arg(ap, const char *)); break;
        case 'c': printf("c:%c ", (char)va_arg(ap, int)); break;
        }
    }
    va_end(ap);
    printf("\n");
}

static int my_format(char *out, size_t n, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    vsnprintf(out, n, fmt, ap);
    va_end(ap);
    return len;
}

int main(void)
{
    printf("ints %d %u %x %o %c %%\n", -42, 42u, 0xABCu, 8, 'Z');
    printf("width [%5d] [%-5d] [%05d] [%+d]\n", 42, 42, 42, 42);
    printf("str [%s] [%8s] [%-8s] [%.3s]\n", "pico", "pico", "pico", "picocalc");
    printf("float %f %.2f %8.3f %-8.1f|\n", 3.14159, 2.0 / 3.0, -1.5, 0.3);
    printf("mixed %d %.1f %d %.1f %s %lld\n", 1, 2.5, 3, 4.5, "five", 6000000000LL);
    printf("long %ld %lu\n", -100000L, 4000000000UL);

    printf("sum %d %d %d\n", sum_ints(0), sum_ints(3, 1, 2, 3), sum_ints(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    printf("avg %.3f\n", avg(4, 1.0, 2.0, 3.5, 4.5));
    typed("idslci", 7, 1.25, "txt", 1LL << 40, 'q', -1);

    char buf[32];
    int len = my_format(buf, sizeof buf, "%s=%d (%.1f%%)", "load", 75, 75.0);
    printf("vsn %s %d %d\n", buf, len, (int)strlen(buf));
    return 0;
}
