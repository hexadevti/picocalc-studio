/* t13: double - under ilp32f doubles are passed in integer register pairs and computed by libgcc
 * soft-float helpers (__adddf3, __muldf3, ...). Also float<->double promotion and <math.h>. */
#include <stdio.h>
#include <math.h>

static double hyp(double a, double b) { return sqrt(a * a + b * b); }
static double mixfd(float f, double d, int i) { return f * d + i; }
static double many(double a, double b, double c, double d, double e) { return a - b + c - d + e; }  /* e on stack */
static float narrow(double d) { return (float)d; }

int main(void)
{
    volatile double a = 1.0 / 3.0, b = 2.75;
    printf("ops %.10f %.6f %.6f %.6f\n", a, a + b, a * b, b / a);
    printf("cmp %d %d %d\n", a < b, a == 1.0 / 3.0, b >= 2.75);
    printf("hyp %.6f\n", hyp(3.0, 4.0));
    printf("mixfd %.4f\n", mixfd(0.5f, 10.25, 3));
    printf("many %.2f\n", many(10.0, 1.5, 2.25, 0.75, 100.0));
    printf("narrow %.3f\n", (double)narrow(2.0 / 3.0));

    printf("math %.6f %.6f %.6f %.6f\n", sin(1.0), cos(1.0), tan(0.5), atan2(1.0, -1.0));
    printf("exp %.6f %.6f %.6f %.6f\n", exp(1.0), log(10.0), pow(2.0, 0.5), log10(1000.0));
    printf("round %.1f %.1f %.1f %.1f %.1f\n", floor(-2.5), ceil(-2.5), fabs(-7.3), fmod(10.0, 3.0), round(2.5));

    volatile double big = 1e300;
    printf("big %d %d\n", isinf(big * 1e10) != 0, isnan(sqrt(-1.0)) != 0);
    printf("conv %d %u %lld\n", (int)-7.9, (unsigned)3e9, (long long)-1e15);
    printf("fromint %.1f %.1f\n", (double)0x7fffffff, (double)-9000000000LL);

    double pi = 0.0;
    for (int k = 0; k < 10000; k++) pi += (k % 2 ? -4.0 : 4.0) / (2 * k + 1);
    printf("leibniz %.5f\n", pi);

    printf("fmt %e %g %g %.3g\n", 12345.678, 0.0001, 1e20, 3.14159);
    return 0;
}
