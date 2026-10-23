/* math.h - PicoCalc C library (the firmware's newlib libm; only exported functions declared)
 *
 * float runs on the FPU, double is software-emulated: prefer the f-suffixed functions.
 * The classification macros are plain C and evaluate their argument more than once. */
#ifndef _MATH_H
#define _MATH_H

typedef float float_t;
typedef double double_t;

#define HUGE_VAL  (1e300 * 1e300)
#define HUGE_VALF ((float)(1e300 * 1e300))
#define HUGE_VALL ((long double)(1e300 * 1e300))
#define INFINITY  ((float)(1e300 * 1e300))
#define NAN       (0.0f / 0.0f)

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define MATH_ERRNO       1
#define MATH_ERREXCEPT   2
#define math_errhandling (MATH_ERRNO | MATH_ERREXCEPT)

#define isnan(x)    ((x) != (x))
#define isinf(x)    (!isnan(x) && isnan((x) - (x)))
#define isfinite(x) (!isnan((x) - (x)))
#define isnormal(x) (fpclassify(x) == FP_NORMAL)
#define signbit(x)                                                                    \
    (sizeof(x) == sizeof(float) ? (int)(((union { float __f; unsigned int __u; }){(x)}).__u >> 31) \
                                : (int)(((union { double __d; unsigned long long __u; }){(x)}).__u >> 63))
#define fpclassify(x)                                                                 \
    (isnan(x) ? FP_NAN : isinf(x) ? FP_INFINITE : (x) == 0 ? FP_ZERO :                \
     ((x) < 0 ? -(x) : (x)) < (sizeof(x) == sizeof(float) ? 1.17549435082228750797e-38F \
                                                          : 2.22507385850720138309e-308) \
         ? FP_SUBNORMAL : FP_NORMAL)

#define isunordered(a, b)    (isnan(a) || isnan(b))
#define isgreater(a, b)      (!isunordered(a, b) && (a) > (b))
#define isgreaterequal(a, b) (!isunordered(a, b) && (a) >= (b))
#define isless(a, b)         (!isunordered(a, b) && (a) < (b))
#define islessequal(a, b)    (!isunordered(a, b) && (a) <= (b))
#define islessgreater(a, b)  (!isunordered(a, b) && (a) != (b))

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.693147180559945309417
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440
#define M_TWOPI    (M_PI * 2.0)

/* double */
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double sqrt(double x);
double pow(double x, double y);
double exp(double x);
double log(double x);
double log10(double x);
double log2(double x);
double fmod(double x, double y);
double floor(double x);
double ceil(double x);
double fabs(double x);
double round(double x);
double trunc(double x);
double hypot(double x, double y);

/* float */
float sinf(float x);
float cosf(float x);
float tanf(float x);
float atan2f(float y, float x);
float sqrtf(float x);
float powf(float x, float y);
float expf(float x);
float logf(float x);
float fmodf(float x, float y);
float floorf(float x);
float ceilf(float x);
float fabsf(float x);
float roundf(float x);

#endif
