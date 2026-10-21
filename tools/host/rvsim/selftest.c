/*
 * selftest.c - RV32 guest program that checks rvsim against the RISC-V spec on the corner cases
 * the C unit tests barely touch: M-extension edge cases, every F rounding mode, fflags, NaN
 * rules, saturating fcvt, fused multiply-add, fclass, A extension, counters.
 * Expected values are written down from the ISA manual, not taken from the emulator.
 *
 *   python tools/host/simfw/build.py link -o build/host/selftest.elf tools/host/rvsim/selftest.c
 *   build/host/rvsim.exe build/host/selftest.elf        (exit status = number of failures)
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;

static void check(const char *what, uint32_t got, uint32_t want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %-34s got 0x%08lx want 0x%08lx\n", what, (unsigned long)got, (unsigned long)want);
    }
}

static float bf(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t fb(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

static uint32_t get_fflags(void) { uint32_t v; __asm__ volatile("frflags %0" : "=r"(v)); return v; }
static void clr_fflags(void) { __asm__ volatile("fsflags zero"); }

#define NX 1
#define UF 2
#define OF 4
#define DZ 8
#define NV 16
#define QNAN 0x7fc00000u

/* r = op a, b with a static rounding mode */
#define FOP(op, rm, a, b) ({ float _r; __asm__ volatile(op " %0, %1, %2, " rm : "=f"(_r) : "f"(a), "f"(b)); fb(_r); })
#define FCVT_W(op, rm, a) ({ uint32_t _r; __asm__ volatile(op " %0, %1, " rm : "=r"(_r) : "f"(a)); _r; })
#define FCVT_S(op, rm, a) ({ float _r; __asm__ volatile(op " %0, %1, " rm : "=f"(_r) : "r"(a)); fb(_r); })
#define FMA(op, rm, a, b, c) ({ float _r; __asm__ volatile(op " %0, %1, %2, %3, " rm : "=f"(_r) : "f"(a), "f"(b), "f"(c)); fb(_r); })
#define F2(op, a, b) ({ float _r; __asm__ volatile(op " %0, %1, %2" : "=f"(_r) : "f"(a), "f"(b)); fb(_r); })
#define FCMP(op, a, b) ({ uint32_t _r; __asm__ volatile(op " %0, %1, %2" : "=r"(_r) : "f"(a), "f"(b)); _r; })

static void test_m(void)
{
    volatile int32_t mn = INT32_MIN, m1 = -1, z = 0, seven = 7, mseven = -7, two = 2;
    volatile uint32_t big = 0xffffffffu, uz = 0;
    check("div INT_MIN/-1", (uint32_t)(mn / m1), 0x80000000u);
    check("rem INT_MIN%-1", (uint32_t)(mn % m1), 0);
    check("div x/0", (uint32_t)(seven / z), 0xffffffffu);
    check("rem x%0", (uint32_t)(seven % z), 7);
    check("divu x/0", big / uz, 0xffffffffu);
    check("remu x%0", big % uz, 0xffffffffu);
    check("div -7/2", (uint32_t)(mseven / two), (uint32_t)-3);
    check("rem -7%2", (uint32_t)(mseven % two), (uint32_t)-1);
    uint32_t r;
    __asm__("mulh %0,%1,%2" : "=r"(r) : "r"(-5), "r"(3));
    check("mulh -5*3", r, 0xffffffffu);
    __asm__("mulh %0,%1,%2" : "=r"(r) : "r"(INT32_MIN), "r"(INT32_MIN));
    check("mulh MIN*MIN", r, 0x40000000u);
    __asm__("mulhu %0,%1,%2" : "=r"(r) : "r"(0xffffffffu), "r"(0xffffffffu));
    check("mulhu max*max", r, 0xfffffffeu);
    __asm__("mulhsu %0,%1,%2" : "=r"(r) : "r"(-1), "r"(0xffffffffu));
    check("mulhsu -1*max", r, 0xffffffffu);
    __asm__("mulhsu %0,%1,%2" : "=r"(r) : "r"(2), "r"(0x80000000u));
    check("mulhsu 2*2^31", r, 1);
    __asm__("sra %0,%1,%2" : "=r"(r) : "r"(0x80000000u), "r"(33));   /* shift amount mod 32 */
    check("sra by 33", r, 0xc0000000u);
}

static void test_fp_rounding(void)
{
    float one = 1.0f, half_ulp = bf(0x33800000), mone = -1.0f;     /* 2^-24 = half ulp of 1.0 */
    check("fadd tie rne", FOP("fadd.s", "rne", one, half_ulp), 0x3f800000);
    check("fadd tie rtz", FOP("fadd.s", "rtz", one, half_ulp), 0x3f800000);
    check("fadd tie rdn", FOP("fadd.s", "rdn", one, half_ulp), 0x3f800000);
    check("fadd tie rup", FOP("fadd.s", "rup", one, half_ulp), 0x3f800001);
    check("fadd tie rmm", FOP("fadd.s", "rmm", one, half_ulp), 0x3f800001);
    check("fsub -tie rdn", FOP("fsub.s", "rdn", mone, half_ulp), 0xbf800001);
    check("fsub -tie rmm", FOP("fsub.s", "rmm", mone, half_ulp), 0xbf800001);
    check("fsub -tie rne", FOP("fsub.s", "rne", mone, half_ulp), 0xbf800000);
    /* 1 + 3*2^-25 = 1.75 half-ulps: all nearest modes round up */
    float x = bf(0x33c00000);                                         /* 1.5 * 2^-24 */
    check("fadd 0.75ulp rne", FOP("fadd.s", "rne", one, x), 0x3f800001);
    check("fadd 0.75ulp rtz", FOP("fadd.s", "rtz", one, x), 0x3f800000);
    /* x + (-x) = -0 only in rdn */
    check("x-x rdn = -0", FOP("fsub.s", "rdn", one, one), 0x80000000);
    check("x-x rne = +0", FOP("fsub.s", "rne", one, one), 0x00000000);
    /* division / sqrt */
    float three = 3.0f, two = 2.0f, zero = 0.0f, mfour = -4.0f;
    check("1/3 rne", FOP("fdiv.s", "rne", one, three), 0x3eaaaaab);
    check("1/3 rtz", FOP("fdiv.s", "rtz", one, three), 0x3eaaaaaa);
    check("1/3 rup", FOP("fdiv.s", "rup", one, three), 0x3eaaaaab);
    check("1/3 rmm", FOP("fdiv.s", "rmm", one, three), 0x3eaaaaab);
    { float r; __asm__ volatile("fsqrt.s %0,%1,rne" : "=f"(r) : "f"(two)); check("sqrt 2 rne", fb(r), 0x3fb504f3); }
    { float r; __asm__ volatile("fsqrt.s %0,%1,rup" : "=f"(r) : "f"(two)); check("sqrt 2 rup", fb(r), 0x3fb504f4); }
    clr_fflags();
    { float r; __asm__ volatile("fsqrt.s %0,%1" : "=f"(r) : "f"(mfour)); check("sqrt -4 = qNaN", fb(r), QNAN); }
    check("sqrt -4 flags", get_fflags(), NV);
    /* flags */
    clr_fflags();
    check("1/0 = inf", FOP("fdiv.s", "rne", one, zero), 0x7f800000);
    check("1/0 flags", get_fflags(), DZ);
    clr_fflags();
    check("0/0 = canonical NaN", FOP("fdiv.s", "rne", zero, zero), QNAN);
    check("0/0 flags", get_fflags(), NV);
    clr_fflags();
    FOP("fdiv.s", "rne", one, three);
    check("1/3 flags", get_fflags(), NX);
    clr_fflags();
    float maxf = bf(0x7f7fffff);
    check("max*2 rne = inf", FOP("fmul.s", "rne", maxf, two), 0x7f800000);
    check("overflow flags", get_fflags(), OF | NX);
    check("max*2 rtz = max", FOP("fmul.s", "rtz", maxf, two), 0x7f7fffff);
    clr_fflags();
    float tiny = bf(0x00800001);                                      /* smallest normal + 1ulp */
    check("tiny/2 (subnormal)", FOP("fdiv.s", "rne", tiny, two), 0x00400000);
    check("underflow flags", get_fflags(), UF | NX);
    /* NaN payloads are not propagated */
    float pnan = bf(0x7fc12345);
    check("qNaN+1 canonical", FOP("fadd.s", "rne", pnan, one), QNAN);
    /* dynamic rounding mode via frm */
    __asm__ volatile("fsrm %0" :: "r"(3));                             /* rup */
    { float r; __asm__ volatile("fadd.s %0,%1,%2" : "=f"(r) : "f"(one), "f"(half_ulp)); check("frm=rup dyn add", fb(r), 0x3f800001); }
    uint32_t fcsr;
    __asm__ volatile("frcsr %0" : "=r"(fcsr));
    check("fcsr frm field", fcsr >> 5, 3);
    __asm__ volatile("fsrm zero");
    __asm__ volatile("fscsr %0" :: "r"(0xff));
    __asm__ volatile("frcsr %0" : "=r"(fcsr));
    check("fcsr mask", fcsr, 0xff);
    __asm__ volatile("fscsr zero");
}

static void test_fp_convert(void)
{
    float p25 = 2.5f, m25 = -2.5f, nan = bf(QNAN), big = 3e9f, mbig = -3e9f, m1 = -1.0f, mhalf = -0.5f;
    float inf = bf(0x7f800000), minf = bf(0xff800000);
    check("fcvt.w 2.5 rne", FCVT_W("fcvt.w.s", "rne", p25), 2);
    check("fcvt.w 2.5 rmm", FCVT_W("fcvt.w.s", "rmm", p25), 3);
    check("fcvt.w 2.5 rup", FCVT_W("fcvt.w.s", "rup", p25), 3);
    check("fcvt.w 2.5 rdn", FCVT_W("fcvt.w.s", "rdn", p25), 2);
    check("fcvt.w 2.5 rtz", FCVT_W("fcvt.w.s", "rtz", p25), 2);
    check("fcvt.w -2.5 rne", FCVT_W("fcvt.w.s", "rne", m25), (uint32_t)-2);
    check("fcvt.w -2.5 rmm", FCVT_W("fcvt.w.s", "rmm", m25), (uint32_t)-3);
    check("fcvt.w -2.5 rdn", FCVT_W("fcvt.w.s", "rdn", m25), (uint32_t)-3);
    check("fcvt.w -2.5 rup", FCVT_W("fcvt.w.s", "rup", m25), (uint32_t)-2);
    clr_fflags();
    check("fcvt.w NaN", FCVT_W("fcvt.w.s", "rtz", nan), 0x7fffffff);
    check("fcvt.w NaN flags", get_fflags(), NV);
    check("fcvt.w 3e9", FCVT_W("fcvt.w.s", "rtz", big), 0x7fffffff);
    check("fcvt.w -3e9", FCVT_W("fcvt.w.s", "rtz", mbig), 0x80000000);
    check("fcvt.w +inf", FCVT_W("fcvt.w.s", "rtz", inf), 0x7fffffff);
    check("fcvt.w -inf", FCVT_W("fcvt.w.s", "rtz", minf), 0x80000000);
    check("fcvt.wu 3e9", FCVT_W("fcvt.wu.s", "rtz", big), 3000000000u);
    check("fcvt.wu NaN", FCVT_W("fcvt.wu.s", "rtz", nan), 0xffffffff);
    check("fcvt.wu +inf", FCVT_W("fcvt.wu.s", "rtz", inf), 0xffffffff);
    clr_fflags();
    check("fcvt.wu -1", FCVT_W("fcvt.wu.s", "rtz", m1), 0);
    check("fcvt.wu -1 flags", get_fflags(), NV);
    clr_fflags();
    check("fcvt.wu -0.5 rtz", FCVT_W("fcvt.wu.s", "rtz", mhalf), 0);
    check("fcvt.wu -0.5 flags", get_fflags(), NX);
    clr_fflags();
    check("fcvt.w 2.5 flags", (FCVT_W("fcvt.w.s", "rtz", p25), get_fflags()), NX);
    check("fcvt.s.w 2^24+1 rne", FCVT_S("fcvt.s.w", "rne", 16777217), 0x4b800000);
    check("fcvt.s.w 2^24+1 rup", FCVT_S("fcvt.s.w", "rup", 16777217), 0x4b800001);
    check("fcvt.s.w 2^24+1 rmm", FCVT_S("fcvt.s.w", "rmm", 16777217), 0x4b800001);
    check("fcvt.s.w -2^24-1 rdn", FCVT_S("fcvt.s.w", "rdn", -16777217), 0xcb800001);
    check("fcvt.s.w INT_MIN", FCVT_S("fcvt.s.w", "rne", INT32_MIN), 0xcf000000);
    check("fcvt.s.wu UINT_MAX", FCVT_S("fcvt.s.wu", "rne", 0xffffffffu), 0x4f800000);
    check("fcvt.s.wu UINT_MAX rtz", FCVT_S("fcvt.s.wu", "rtz", 0xffffffffu), 0x4f7fffff);
}

static void test_fp_misc(void)
{
    float pz = 0.0f, mz = bf(0x80000000), one = 1.0f, two = 2.0f, qn = bf(QNAN), sn = bf(0x7f800001);
    check("fmin(-0,+0)", F2("fmin.s", pz, mz), 0x80000000);
    check("fmax(-0,+0)", F2("fmax.s", mz, pz), 0x00000000);
    clr_fflags();
    check("fmin(qNaN,1)", F2("fmin.s", qn, one), 0x3f800000);
    check("fmin qNaN flags", get_fflags(), 0);
    check("fmax(1,sNaN)", F2("fmax.s", one, sn), 0x3f800000);
    check("fmax sNaN flags", get_fflags(), NV);
    check("fmin(NaN,NaN)", F2("fmin.s", qn, sn), QNAN);
    check("fmax(1,2)", F2("fmax.s", one, two), 0x40000000);
    clr_fflags();
    check("feq qNaN", FCMP("feq.s", qn, qn), 0);
    check("feq qNaN flags", get_fflags(), 0);
    check("flt qNaN", FCMP("flt.s", qn, one), 0);
    check("flt qNaN flags", get_fflags(), NV);
    clr_fflags();
    check("feq sNaN flags", (FCMP("feq.s", sn, one), get_fflags()), NV);
    check("feq -0 +0", FCMP("feq.s", mz, pz), 1);
    check("flt -0 +0", FCMP("flt.s", mz, pz), 0);
    check("fle 1 2", FCMP("fle.s", one, two), 1);
    check("fsgnj", F2("fsgnj.s", one, mz), 0xbf800000);
    check("fsgnjn", F2("fsgnjn.s", one, one), 0xbf800000);
    check("fsgnjx", F2("fsgnjx.s", bf(0xbf800000), mz), 0x3f800000);
    check("fsgnj keeps NaN payload", F2("fsgnj.s", bf(0x7fc12345), one), 0x7fc12345);
    static const uint32_t cls_in[10] = { 0xff800000, 0xbf800000, 0x80000001, 0x80000000, 0x00000000,
                                         0x00000001, 0x3f800000, 0x7f800000, 0x7f800001, 0x7fc00000 };
    for (int i = 0; i < 10; i++) {
        uint32_t r;
        __asm__ volatile("fclass.s %0,%1" : "=r"(r) : "f"(bf(cls_in[i])));
        check("fclass", r, 1u << i);
    }
    uint32_t v;
    __asm__ volatile("fmv.w.x %0,%1\n\tfmv.x.w %1,%0" : "=&f"(qn), "=r"(v) : "1"(0x7f812345u));
    check("fmv keeps sNaN bits", v, 0x7f812345);
}

static void test_fma(void)
{
    float a = bf(0x3f800800), c = bf(0xbf801000);   /* a = 1+2^-12, c = -(1+2^-11) */
    /* a*a = 1 + 2^-11 + 2^-24 exactly; fused with c leaves 2^-24 */
    check("fmadd fused", FMA("fmadd.s", "rne", a, a, c), 0x33800000);
    check("fmsub fused", FMA("fmsub.s", "rne", a, a, bf(0x3f801000)), 0x33800000);
    check("fnmsub fused", FMA("fnmsub.s", "rne", a, a, bf(0x3f801000)), 0xb3800000);
    check("fnmadd fused", FMA("fnmadd.s", "rne", a, a, c), 0xb3800000);
    float one = 1.0f, mone = -1.0f, zero = 0.0f, inf = bf(0x7f800000), qn = bf(QNAN);
    check("fmadd 1*1-1 rdn = -0", FMA("fmadd.s", "rdn", one, one, mone), 0x80000000);
    check("fmadd 1*1-1 rne = +0", FMA("fmadd.s", "rne", one, one, mone), 0x00000000);
    /* rounding of the fused result: 1*1 + 2^-24 is a tie */
    float hu = bf(0x33800000);
    check("fmadd tie rne", FMA("fmadd.s", "rne", one, one, hu), 0x3f800000);
    check("fmadd tie rmm", FMA("fmadd.s", "rmm", one, one, hu), 0x3f800001);
    check("fmadd tie rup", FMA("fmadd.s", "rup", one, one, hu), 0x3f800001);
    /* sticky: tie + a tiny bit more must round up even in rne */
    check("fmadd tie+eps rne", FMA("fmadd.s", "rne", bf(0x33800001), one, one), 0x3f800001);
    clr_fflags();
    check("fmadd inf*0+qNaN", FMA("fmadd.s", "rne", inf, zero, qn), QNAN);
    check("fmadd inf*0 flags", get_fflags(), NV);
    clr_fflags();
    check("fmadd inf-inf", FMA("fmadd.s", "rne", inf, one, bf(0xff800000)), QNAN);
    check("fmadd inf-inf flags", get_fflags(), NV);
    clr_fflags();
    check("fmadd exact", FMA("fmadd.s", "rne", bf(0x40000000), bf(0x40400000), one), 0x40e00000);
    check("fmadd exact flags", get_fflags(), 0);
}

static void test_atomics_csr(void)
{
    static volatile uint32_t word = 10;
    uint32_t old, sc;
    __asm__ volatile("amoadd.w %0,%2,(%1)" : "=r"(old) : "r"(&word), "r"(5) : "memory");
    check("amoadd old", old, 10);
    check("amoadd new", word, 15);
    __asm__ volatile("amoswap.w %0,%2,(%1)" : "=r"(old) : "r"(&word), "r"(-3) : "memory");
    check("amoswap", old, 15);
    __asm__ volatile("amomin.w %0,%2,(%1)" : "=r"(old) : "r"(&word), "r"(4) : "memory");
    check("amomin signed", word, (uint32_t)-3);
    __asm__ volatile("amominu.w %0,%2,(%1)" : "=r"(old) : "r"(&word), "r"(4) : "memory");
    check("amominu", word, 4);
    __asm__ volatile("amomax.w %0,%2,(%1)" : "=r"(old) : "r"(&word), "r"(-9) : "memory");
    check("amomax", word, 4);
    __asm__ volatile("amomaxu.w %0,%2,(%1)" : "=r"(old) : "r"(&word), "r"(-9) : "memory");
    check("amomaxu", word, (uint32_t)-9);
    __asm__ volatile("amoor.w %0,%2,(%1)\n\tamoand.w %0,%3,(%1)\n\tamoxor.w %0,%4,(%1)"
                     : "=&r"(old) : "r"(&word), "r"(0xf0), "r"(0xff), "r"(0x0f) : "memory");
    check("amoor/and/xor", word, 0xf8);   /* 0xfffffff7 | 0xf0 & 0xff ^ 0x0f */
    __asm__ volatile("lr.w %0,(%2)\n\tsc.w %1,%3,(%2)" : "=&r"(old), "=&r"(sc) : "r"(&word), "r"(77) : "memory");
    check("lr.w", old, 0xf8);
    check("sc.w success", sc, 0);
    check("sc.w stored", word, 77);
    __asm__ volatile("sc.w %0,%2,(%1)" : "=r"(sc) : "r"(&word), "r"(1) : "memory");
    check("sc.w without lr fails", sc, 1);
    uint32_t c0, c1, i0, i1;
    __asm__ volatile("rdcycle %0\n\trdinstret %1\n\tnop\n\tnop\n\trdcycle %2\n\trdinstret %3"
                     : "=r"(c0), "=r"(i0), "=r"(c1), "=r"(i1));
    check("rdinstret delta", i1 - i0, 4);
    check("rdcycle advances", c1 > c0, 1);
}

int main(void)
{
    test_m();
    test_fp_rounding();
    test_fp_convert();
    test_fp_misc();
    test_fma();
    test_atomics_csr();
    printf("rvsim selftest: %d checks, %d failures\n", checks, failures);
    return failures;
}
