/* t07: struct layout (ilp32f), nested structs, pass/return by value (small, large, float-only,
 * mixed float/int - the ilp32f ABI puts these in different registers), bitfields, copies. */
#include <stdio.h>
#include <stddef.h>
#include <string.h>

struct A { char c; int i; short s; };
struct B { char c; double d; };
struct C { char c; long long ll; };
struct D { char a; char b; };
struct E { int x; char tail[3]; };
struct Outer { char tag; struct A a; struct D d; };
struct Bits { unsigned a : 3; unsigned b : 5; unsigned c : 10; int d : 6; };

/* Layout must match riscv32-esp-elf-gcc -mabi=ilp32f; the reference compiler checks these too. */
_Static_assert(sizeof(struct A) == 12, "A");
_Static_assert(sizeof(struct B) == 16, "B");
_Static_assert(sizeof(struct C) == 16, "C");
_Static_assert(sizeof(struct D) == 2, "D");
_Static_assert(sizeof(struct E) == 8, "E");
_Static_assert(sizeof(struct Outer) == 20, "Outer");
_Static_assert(sizeof(struct Bits) == 4, "Bits");
_Static_assert(offsetof(struct B, d) == 8, "B.d");
_Static_assert(offsetof(struct Outer, d) == 16, "Outer.d");

struct Small { short a; short b; int c; };          /* 8 bytes: two integer registers */
struct Large { int v[6]; };                         /* 24 bytes: passed by reference */
struct Vec2f { float x, y; };                       /* two FP registers */
struct FI { float f; int i; };                      /* one FP + one integer register */
struct Pt { int x, y; };

static struct Small make_small(int a, int b, int c) { struct Small s = { (short)a, (short)b, c }; return s; }
static int sum_small(struct Small s) { return s.a + s.b + s.c; }
static struct Large make_large(int base) { struct Large l; for (int i = 0; i < 6; i++) l.v[i] = base + i; return l; }
static int sum_large(struct Large l) { int s = 0; for (int i = 0; i < 6; i++) s += l.v[i]; l.v[0] = 999; return s; }
static struct Vec2f vadd(struct Vec2f a, struct Vec2f b) { struct Vec2f r = { a.x + b.x, a.y + b.y }; return r; }
static struct FI fi_scale(struct FI v, int k) { v.f *= (float)k; v.i *= k; return v; }
static void move_pt(struct Pt *p, int dx, int dy) { p->x += dx; p->y += dy; }

int main(void)
{
    printf("sizes %d %d %d %d %d %d %d\n", (int)sizeof(struct A), (int)sizeof(struct B), (int)sizeof(struct C),
           (int)sizeof(struct D), (int)sizeof(struct E), (int)sizeof(struct Outer), (int)sizeof(struct Bits));
    printf("offs %d %d %d %d %d\n", (int)offsetof(struct A, i), (int)offsetof(struct A, s), (int)offsetof(struct B, d),
           (int)offsetof(struct C, ll), (int)offsetof(struct Outer, d));

    struct Outer o;
    memset(&o, 0, sizeof o);
    o.tag = 'T'; o.a.c = 'x'; o.a.i = 123456; o.a.s = -7; o.d.a = 1; o.d.b = 2;
    struct Outer o2 = o;          /* struct copy */
    o.a.i = 0;
    printf("nested %c %c %d %d %d %d\n", o2.tag, o2.a.c, o2.a.i, o2.a.s, o2.d.a + o2.d.b, o.a.i);

    struct Bits bits;
    memset(&bits, 0, sizeof bits);
    bits.a = 5; bits.b = 17; bits.c = 1000; bits.d = -20;
    unsigned raw;
    memcpy(&raw, &bits, sizeof raw);
    printf("bits %u %u %u %d raw %08x\n", bits.a, bits.b, bits.c, bits.d, raw);
    bits.a = 9;                   /* truncates to 3 bits */
    bits.c++;
    printf("bits2 %u %u\n", bits.a, bits.c);

    struct Small s = make_small(100, -50, 70000);
    printf("small %d %d %d %d\n", s.a, s.b, s.c, sum_small(s));

    struct Large l = make_large(10);
    printf("large %d %d %d\n", sum_large(l), l.v[0], l.v[5]);

    struct Vec2f v1 = { 1.5f, 2.25f }, v2 = { -0.5f, 4.0f };
    struct Vec2f v3 = vadd(v1, v2);
    printf("vec %.3f %.3f\n", v3.x, v3.y);

    struct FI fi = { 2.5f, 3 };
    fi = fi_scale(fi, 4);
    printf("fi %.2f %d\n", fi.f, fi.i);

    struct Pt pts[3] = { { 1, 2 }, { 3, 4 }, { 5, 6 } };
    for (int i = 0; i < 3; i++) move_pt(&pts[i], 10, 100);
    struct Pt *pp = pts;
    printf("pts %d %d %d %d\n", pts[0].x, pts[2].y, (pp + 1)->x, (*pp).y);

    struct Pt cl = (struct Pt){ .y = 9, .x = 8 };   /* compound literal + designated init */
    printf("designated %d %d\n", cl.x, cl.y);
    return 0;
}
