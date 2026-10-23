/* t12: single-precision float - lives in FP registers under ilp32f (fa0..fa7), hardware F extension. */
#include <stdio.h>

static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float sum9(float a, float b, float c, float d, float e, float f, float g, float h, float i)
{
    /* a..h in fa0..fa7, i spills to an integer register / stack */
    return a + b + c + d + e + f + g + h + i;
}
static float mixed(int a, float b, int c, float d) { return (float)a * b + (float)c * d; }
static float fabsf_(float x) { return x < 0 ? -x : x; }
static float sqrt_newton(float x)
{
    float g = x / 2.0f;
    for (int i = 0; i < 20; i++) g = 0.5f * (g + x / g);
    return g;
}

int main(void)
{
    volatile float a = 3.5f, b = -1.25f;
    printf("ops %.4f %.4f %.4f %.4f\n", a + b, a - b, a * b, a / b);
    printf("cmp %d %d %d %d\n", a > b, a < b, a == 3.5f, b != b);
    printf("lerp %.3f\n", lerp(10.0f, 20.0f, 0.25f));
    printf("sum9 %.1f\n", sum9(1, 2, 3, 4, 5, 6, 7, 8, 9));
    printf("mixed %.2f\n", mixed(3, 1.5f, -2, 0.25f));
    printf("abs %.2f sqrt %.5f\n", fabsf_(b), sqrt_newton(2.0f));

    /* conversions */
    volatile float big = 3.99f, neg = -3.99f;
    printf("trunc %d %d\n", (int)big, (int)neg);
    printf("u2f %.1f i2f %.1f\n", (float)4000000000u, (float)-123456);
    printf("f2u %u\n", (unsigned)big);
    unsigned char uc = (unsigned char)(float)200.7f;
    printf("f2uc %d\n", uc);

    float acc = 0.0f;
    for (int i = 1; i <= 100; i++) acc += 1.0f / (float)i;
    printf("harmonic %.4f\n", acc);

    float arr[4] = { 0.5f, 1.5f, 2.5f, 3.5f };
    float *fp = arr;
    fp[2] *= 2.0f;
    printf("array %.2f %.2f\n", arr[2], *(fp + 3));

    float x = 1.0f;
    x += 0.5f; x *= 3.0f; x -= 1.0f; x /= 2.0f;
    printf("compound %.3f\n", x);
    return 0;
}
