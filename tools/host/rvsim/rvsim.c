/*
 * rvsim - small RV32IMAFC + Zicsr user-mode emulator for host-side testing.
 *
 * Runs RV32 ELF executables built for the ESP32-P4 (rv32imafc, ABI ilp32f) and linked with the
 * "sim firmware" runtime in tools/host/simfw. The guest gets one flat RAM region (default 64 MB
 * at 0x40000000, stack at the top); every load, store and fetch is bounds-checked. Services such
 * as file I/O are requested with `ecall`, a7 = syscall number (Linux riscv numbers where one
 * exists, a private range for directory iteration; see SYSCALLS below and simfw/sim_syscalls.h).
 * Guest paths under /sd/ map to a host directory (--sd).
 *
 *   rvsim [--trace] [--sd DIR] [--max-insns N] [--mem MB] [--stats] prog.elf [args...]
 *
 * Exit status is the guest's exit status. Emulator-detected faults stop the run with a message
 * on stderr and a status above 128: 132 illegal instruction, 133 ebreak, 134 abort/kill,
 * 139 bad memory access, 124 instruction limit reached, 125 emulator/ELF error.
 *
 * Floating point: single precision only (F). Arithmetic runs on host SSE with MXCSR set to the
 * guest rounding mode, so results and fflags are IEEE-exact; RISC-V specifics (canonical NaN,
 * fmin/fmax NaN rules, saturating fcvt, fused multiply-add, RMM rounding) are handled here.
 * Build with: gcc -O2 -std=gnu11 -frounding-math -ffp-contract=off -fno-math-errno
 */
#define _CRT_SECURE_NO_WARNINGS
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define host_mkdir(p) _mkdir(p)
#else
#include <unistd.h>
#define host_mkdir(p) mkdir(p, 0777)
#define O_BINARY 0
#endif

/* ------------------------------------------------------------------------------------------ */
/* Machine state                                                                              */

#define MEM_BASE 0x40000000u

static uint8_t *mem;
static uint32_t mem_size = 64u << 20;
static uint32_t xr[32];            /* integer registers, xr[0] forced to 0 after each insn */
static uint32_t fr[32];            /* FP registers as raw IEEE single bits (FLEN = 32) */
static uint32_t pc;                /* address of the instruction being executed */
static uint32_t fflags, frm;       /* fcsr = frm << 5 | fflags */
static uint32_t mstatus = 0x00002000; /* FS = initial; FP is always usable */
static uint32_t lr_addr;
static int lr_valid;
static uint64_t instret, max_insns = 20000000000ull;
static int trace;

/* ------------------------------------------------------------------------------------------ */
/* ELF symbols, for error messages                                                            */

typedef struct { uint32_t addr; const char *name; } Sym;
static Sym *syms;
static int nsyms;
static uint8_t *elf_image;

static int sym_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Sym *)a)->addr, y = ((const Sym *)b)->addr;
    return x < y ? -1 : x > y;
}

static const char *sym_at(uint32_t addr, char *buf, size_t n)
{
    int lo = 0, hi = nsyms - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (syms[mid].addr <= addr) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0) snprintf(buf, n, "?");
    else snprintf(buf, n, "%s+0x%x", syms[best].name, addr - syms[best].addr);
    return buf;
}

static void die(int code, const char *fmt, ...)
{
    char sb[160];
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "rvsim: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n  pc 0x%08x <%s>, %llu instructions executed\n", pc,
            sym_at(pc, sb, sizeof sb), (unsigned long long)instret);
    exit(code);
}

/* ------------------------------------------------------------------------------------------ */
/* Memory                                                                                     */

static void bad_access(uint32_t addr, uint32_t n, const char *kind)
{
    die(139, "bad memory access: %s of %u byte(s) at 0x%08x (RAM is 0x%08x-0x%08x)", kind, n,
        addr, MEM_BASE, MEM_BASE + mem_size - 1);
}

static inline uint8_t *maddr(uint32_t addr, uint32_t n, const char *kind)
{
    uint32_t off = addr - MEM_BASE;
    if (__builtin_expect(off > mem_size - n, 0)) bad_access(addr, n, kind);
    return mem + off;
}

static inline uint32_t ld8(uint32_t a) { return *maddr(a, 1, "load"); }
static inline uint32_t ld16(uint32_t a) { uint16_t v; memcpy(&v, maddr(a, 2, "load"), 2); return v; }
static inline uint32_t ld32(uint32_t a) { uint32_t v; memcpy(&v, maddr(a, 4, "load"), 4); return v; }
static inline void st8(uint32_t a, uint32_t v) { *maddr(a, 1, "store") = (uint8_t)v; }
static inline void st16(uint32_t a, uint32_t v) { uint16_t h = (uint16_t)v; memcpy(maddr(a, 2, "store"), &h, 2); }
static inline void st32(uint32_t a, uint32_t v) { memcpy(maddr(a, 4, "store"), &v, 4); }

/* Little-endian helpers on host pointers (guest structs filled by syscalls). */
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }

/* A NUL-terminated guest string, bounds-checked. */
static const char *gstr(uint32_t addr)
{
    uint8_t *p = maddr(addr, 1, "string read");
    if (!memchr(p, 0, mem_size - (addr - MEM_BASE))) bad_access(addr, 1, "string read");
    return (const char *)p;
}

/* ------------------------------------------------------------------------------------------ */
/* Compressed instructions: every 16-bit encoding is expanded once, at startup, into the      */
/* equivalent 32-bit instruction (0 = illegal). The executor then only knows 32-bit formats;  */
/* it still uses the real length (2) for pc and link addresses.                               */

static uint32_t cexp[65536];

static uint32_t enc_r(uint32_t op, uint32_t rd, uint32_t f3, uint32_t rs1, uint32_t rs2, uint32_t f7)
{ return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op; }
static uint32_t enc_i(uint32_t op, uint32_t rd, uint32_t f3, uint32_t rs1, int32_t imm)
{ return ((uint32_t)imm & 0xfff) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op; }
static uint32_t enc_s(uint32_t op, uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm)
{ return ((uint32_t)imm >> 5 & 0x7f) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | ((uint32_t)imm & 0x1f) << 7 | op; }
static uint32_t enc_b(uint32_t f3, uint32_t rs1, uint32_t rs2, int32_t imm)
{
    uint32_t u = (uint32_t)imm;
    return (u >> 12 & 1) << 31 | (u >> 5 & 0x3f) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 |
           (u >> 1 & 0xf) << 8 | (u >> 11 & 1) << 7 | 0x63;
}
static uint32_t enc_j(uint32_t rd, int32_t imm)
{
    uint32_t u = (uint32_t)imm;
    return (u >> 20 & 1) << 31 | (u >> 1 & 0x3ff) << 21 | (u >> 11 & 1) << 20 | (u >> 12 & 0xff) << 12 |
           rd << 7 | 0x6f;
}

static inline uint32_t bit(uint32_t v, int b) { return v >> b & 1; }
static inline int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

static uint32_t expand_c(uint32_t h)
{
    uint32_t f3 = h >> 13, rd = h >> 7 & 31, rs2 = h >> 2 & 31;
    uint32_t rdp = (h >> 2 & 7) + 8, rs1p = (h >> 7 & 7) + 8;   /* x8..x15 */
    int32_t imm6 = sext(bit(h, 12) << 5 | (h >> 2 & 31), 6);
    switch (h & 3) {
    case 0:
        switch (f3) {
        case 0: { /* c.addi4spn */
            uint32_t u = (h >> 11 & 3) << 4 | (h >> 7 & 15) << 6 | bit(h, 6) << 2 | bit(h, 5) << 3;
            return u ? enc_i(0x13, rdp, 0, 2, (int32_t)u) : 0;
        }
        case 2: case 3: { /* c.lw, c.flw */
            uint32_t u = (h >> 10 & 7) << 3 | bit(h, 6) << 2 | bit(h, 5) << 6;
            return enc_i(f3 == 2 ? 0x03 : 0x07, rdp, 2, rs1p, (int32_t)u);
        }
        case 6: case 7: { /* c.sw, c.fsw */
            uint32_t u = (h >> 10 & 7) << 3 | bit(h, 6) << 2 | bit(h, 5) << 6;
            return enc_s(f3 == 6 ? 0x23 : 0x27, 2, rs1p, rdp, (int32_t)u);
        }
        default: return 0; /* c.fld/c.fsd need D; 4 is reserved */
        }
    case 1:
        switch (f3) {
        case 0: return enc_i(0x13, rd, 0, rd, imm6);                 /* c.addi / c.nop */
        case 1: case 5: {                                            /* c.jal / c.j */
            uint32_t u = bit(h, 12) << 11 | bit(h, 11) << 4 | (h >> 9 & 3) << 8 | bit(h, 8) << 10 |
                         bit(h, 7) << 6 | bit(h, 6) << 7 | (h >> 3 & 7) << 1 | bit(h, 2) << 5;
            return enc_j(f3 == 1 ? 1 : 0, sext(u, 12));
        }
        case 2: return enc_i(0x13, rd, 0, 0, imm6);                  /* c.li */
        case 3:
            if (rd == 2) {                                           /* c.addi16sp */
                uint32_t u = bit(h, 12) << 9 | bit(h, 6) << 4 | bit(h, 5) << 6 | (h >> 3 & 3) << 7 |
                             bit(h, 2) << 5;
                return u ? enc_i(0x13, 2, 0, 2, sext(u, 10)) : 0;
            }
            if (imm6 == 0) return 0;                                 /* c.lui */
            return (uint32_t)imm6 << 12 | rd << 7 | 0x37;
        case 4:
            switch (h >> 10 & 3) {
            case 0: return bit(h, 12) ? 0 : enc_r(0x13, rs1p, 5, rs1p, h >> 2 & 31, 0);    /* c.srli */
            case 1: return bit(h, 12) ? 0 : enc_r(0x13, rs1p, 5, rs1p, h >> 2 & 31, 0x20); /* c.srai */
            case 2: return enc_i(0x13, rs1p, 7, rs1p, imm6);                               /* c.andi */
            default: {
                static const uint32_t f3s[4] = { 0, 4, 6, 7 };       /* c.sub c.xor c.or c.and */
                uint32_t k = h >> 5 & 3;
                if (bit(h, 12)) return 0;                            /* RV64 / Zcb encodings */
                return enc_r(0x33, rs1p, f3s[k], rs1p, rdp, k == 0 ? 0x20 : 0);
            }
            }
        default: {                                                   /* c.beqz / c.bnez */
            uint32_t u = bit(h, 12) << 8 | (h >> 10 & 3) << 3 | (h >> 5 & 3) << 6 | (h >> 3 & 3) << 1 |
                         bit(h, 2) << 5;
            return enc_b(f3 == 6 ? 0 : 1, rs1p, 0, sext(u, 9));
        }
        }
    case 2:
        switch (f3) {
        case 0: return bit(h, 12) ? 0 : enc_r(0x13, rd, 1, rd, rs2, 0);  /* c.slli */
        case 2: case 3: {                                                /* c.lwsp, c.flwsp */
            uint32_t u = bit(h, 12) << 5 | (h >> 4 & 7) << 2 | (h >> 2 & 3) << 6;
            if (f3 == 2 && rd == 0) return 0;
            return enc_i(f3 == 2 ? 0x03 : 0x07, rd, 2, 2, (int32_t)u);
        }
        case 4:
            if (!bit(h, 12)) {
                if (rs2 == 0) return rd ? enc_i(0x67, 0, 0, rd, 0) : 0;  /* c.jr */
                return enc_r(0x33, rd, 0, 0, rs2, 0);                     /* c.mv */
            }
            if (rs2 == 0) return rd ? enc_i(0x67, 1, 0, rd, 0) : 0x00100073; /* c.jalr / c.ebreak */
            return enc_r(0x33, rd, 0, rd, rs2, 0);                        /* c.add */
        case 6: case 7: {                                                 /* c.swsp, c.fswsp */
            uint32_t u = (h >> 9 & 15) << 2 | (h >> 7 & 3) << 6;
            return enc_s(f3 == 6 ? 0x23 : 0x27, 2, 2, rs2, (int32_t)u);
        }
        default: return 0;                                               /* c.fldsp/c.fsdsp: D */
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Floating point                                                                             */

enum { RNE, RTZ, RDN, RUP, RMM };
enum { FF_NX = 1, FF_UF = 2, FF_OF = 4, FF_DZ = 8, FF_NV = 16 };
enum { F_ADD, F_SUB, F_MUL, F_DIV, F_SQRT };
#define CANON_NAN 0x7fc00000u

static inline float b2f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline uint32_t f2b(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static inline double u2d(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static inline uint64_t d2u(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static inline int is_nan(uint32_t b) { return (b & 0x7fffffff) > 0x7f800000; }
static inline int is_snan(uint32_t b) { return is_nan(b) && !(b & 0x00400000); }
static inline uint32_t canon(uint32_t b) { return is_nan(b) ? CANON_NAN : b; }

#if defined(__x86_64__) || defined(__i386__)
/* Host SSE: MXCSR gets all exceptions masked plus the rounding control for the guest mode; the
 * status bits read back right after the operation become fflags. The asm blocks keep the op,
 * the mode switch and the status read in order and restore the default MXCSR afterwards. */
static const unsigned mx_rc[4] = { 0u << 13, 3u << 13, 1u << 13, 2u << 13 }; /* RNE RTZ RDN RUP */
static const unsigned mx_default = 0x1f80;
static uint8_t mx2ff[64];

static void fp_host_init(void)
{
    for (int m = 0; m < 64; m++)
        mx2ff[m] = (uint8_t)((m & 1 ? FF_NV : 0) | (m & 4 ? FF_DZ : 0) | (m & 8 ? FF_OF : 0) |
                             (m & 16 ? FF_UF : 0) | (m & 32 ? FF_NX : 0));
}

#define SSE_BIN(insn, T, a, b, rm, fl) do { \
        unsigned _c = mx_default | mx_rc[rm], _s; \
        __asm__ volatile("ldmxcsr %2\n\t" insn " %3, %0\n\tstmxcsr %1\n\tldmxcsr %4" \
                         : "+x"(a), "=m"(_s) : "m"(_c), "x"(b), "m"(mx_default)); \
        *(fl) |= mx2ff[_s & 63]; } while (0)

/* Single or double precision op in rounding mode rm (RNE..RUP); flags are OR-ed into *fl. */
static float host_fs(int op, float a, float b, int rm, uint32_t *fl)
{
    switch (op) {
    case F_ADD: SSE_BIN("addss", float, a, b, rm, fl); break;
    case F_SUB: SSE_BIN("subss", float, a, b, rm, fl); break;
    case F_MUL: SSE_BIN("mulss", float, a, b, rm, fl); break;
    case F_DIV: SSE_BIN("divss", float, a, b, rm, fl); break;
    default:    b = a; SSE_BIN("sqrtss", float, a, b, rm, fl); break;
    }
    return a;
}
static double host_fd(int op, double a, double b, int rm, uint32_t *fl)
{
    switch (op) {
    case F_ADD: SSE_BIN("addsd", double, a, b, rm, fl); break;
    case F_SUB: SSE_BIN("subsd", double, a, b, rm, fl); break;
    case F_MUL: SSE_BIN("mulsd", double, a, b, rm, fl); break;
    case F_DIV: SSE_BIN("divsd", double, a, b, rm, fl); break;
    default:    b = a; SSE_BIN("sqrtsd", double, a, b, rm, fl); break;
    }
    return a;
}
static float host_d2s(double d, int rm, uint32_t *fl)
{
    float r = 0;
    unsigned c = mx_default | mx_rc[rm], s;
    __asm__ volatile("ldmxcsr %2\n\tcvtsd2ss %3, %0\n\tstmxcsr %1\n\tldmxcsr %4"
                     : "+x"(r), "=m"(s) : "m"(c), "x"(d), "m"(mx_default));
    *fl |= mx2ff[s & 63];
    return r;
}
static float host_i2s(int64_t v, int rm, uint32_t *fl)
{
    float r = 0;
    unsigned c = mx_default | mx_rc[rm], s;
#ifdef __x86_64__
    __asm__ volatile("ldmxcsr %2\n\tcvtsi2ssq %3, %0\n\tstmxcsr %1\n\tldmxcsr %4"
                     : "+x"(r), "=m"(s) : "m"(c), "r"(v), "m"(mx_default));
    *fl |= mx2ff[s & 63];
#else
    (void)c; (void)s;
    r = host_d2s((double)v, rm, fl);    /* int32/uint32 are exact in double */
#endif
    return r;
}
#else
/* Portable fallback through <fenv.h> (needs a compiler honouring -frounding-math). */
#include <fenv.h>
static const int fe_rm[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD };
static void fp_host_init(void) {}
static uint32_t fe_flags(void)
{
    int e = fetestexcept(FE_ALL_EXCEPT);
    return (e & FE_INVALID ? FF_NV : 0) | (e & FE_DIVBYZERO ? FF_DZ : 0) | (e & FE_OVERFLOW ? FF_OF : 0) |
           (e & FE_UNDERFLOW ? FF_UF : 0) | (e & FE_INEXACT ? FF_NX : 0);
}
#define FE_DO(T, expr) do { volatile T _r; feclearexcept(FE_ALL_EXCEPT); fesetround(fe_rm[rm]); \
        _r = (expr); *fl |= fe_flags(); fesetround(FE_TONEAREST); return _r; } while (0)
static float host_fs(int op, float a, float b, int rm, uint32_t *fl)
{
    volatile float x = a, y = b;
    switch (op) {
    case F_ADD: FE_DO(float, x + y);
    case F_SUB: FE_DO(float, x - y);
    case F_MUL: FE_DO(float, x * y);
    case F_DIV: FE_DO(float, x / y);
    default:    FE_DO(float, sqrtf(x));
    }
}
static double host_fd(int op, double a, double b, int rm, uint32_t *fl)
{
    volatile double x = a, y = b;
    switch (op) {
    case F_ADD: FE_DO(double, x + y);
    case F_SUB: FE_DO(double, x - y);
    case F_MUL: FE_DO(double, x * y);
    case F_DIV: FE_DO(double, x / y);
    default:    FE_DO(double, sqrt(x));
    }
}
static float host_d2s(double d, int rm, uint32_t *fl) { volatile double x = d; FE_DO(float, (float)x); }
static float host_i2s(int64_t v, int rm, uint32_t *fl) { volatile int64_t x = v; FE_DO(float, (float)x); }
#endif

/* Double -> single with round-to-nearest, ties away from zero (RMM; no SSE equivalent). */
static float d2s_rmm(double d, uint32_t *fl)
{
    uint32_t ftz = 0, fne = 0, faw = 0;
    float tz = host_d2s(d, RTZ, &ftz);
    if (!(ftz & FF_NX)) { *fl |= ftz; return tz; }
    float ne = host_d2s(d, RNE, &fne);
    float aw = host_d2s(d, d > 0 ? RUP : RDN, &faw);
    if (!isinf(aw) && fabs(d - (double)tz) == fabs((double)aw - d)) { *fl |= faw; return aw; }
    *fl |= fne;
    return ne;
}

/* a op b in double, rounded to odd: correct for a later rounding to single in any mode. */
static double d_odd(int op, double a, double b, uint32_t *fl)
{
    uint32_t f = 0;
    double r = host_fd(op, a, b, RTZ, &f);
    if ((f & FF_NX) && isfinite(r)) r = u2d(d2u(r) | 1);
    *fl |= f & (FF_NV | FF_DZ);
    return r;
}

static uint32_t fp_arith(int op, uint32_t a, uint32_t b, int rm)
{
    float r;
    if (rm == RMM) {
        uint32_t fl = 0;
        if (is_nan(a) || (op != F_SQRT && is_nan(b))) {   /* host would quiet; just flag */
            if (is_snan(a) || (op != F_SQRT && is_snan(b))) fflags |= FF_NV;
            return CANON_NAN;
        }
        double d = d_odd(op, b2f(a), b2f(b), &fl);
        r = isnan(d) ? b2f(CANON_NAN) : d2s_rmm(d, &fl);
        fflags |= fl;
    } else {
        r = host_fs(op, b2f(a), b2f(b), rm, &fflags);
    }
    return canon(f2b(r));
}

/* fmadd family: (neg_p ? -(a*b) : a*b) + (neg_c ? -c : c), rounded once. */
static uint32_t fp_fma(uint32_t a, uint32_t b, uint32_t c, int neg_p, int neg_c, int rm)
{
    int a_inf = (a & 0x7fffffff) == 0x7f800000, b_inf = (b & 0x7fffffff) == 0x7f800000;
    int a_zero = !(a & 0x7fffffff), b_zero = !(b & 0x7fffffff);
    if ((a_inf && b_zero) || (a_zero && b_inf)) { fflags |= FF_NV; return CANON_NAN; }
    if (is_nan(a) || is_nan(b) || is_nan(c)) {
        if (is_snan(a) || is_snan(b) || is_snan(c)) fflags |= FF_NV;
        return CANON_NAN;
    }
    double p = (double)b2f(a) * (double)b2f(b);     /* exact: 24x24 bits fits in 53 */
    double cc = b2f(c);
    if (neg_p) p = -p;
    if (neg_c) cc = -cc;
    uint32_t fl = 0;
    double s = d_odd(F_ADD, p, cc, &fl);
    if (isnan(s)) { fflags |= fl; return CANON_NAN; }  /* inf - inf */
    if (s == 0) s = host_fd(F_ADD, p, cc, rm == RMM ? RNE : rm, &fl); /* exact zero: sign by mode */
    float r = rm == RMM ? d2s_rmm(s, &fl) : host_d2s(s, rm, &fl);
    fflags |= fl;
    return canon(f2b(r));
}

static uint32_t fp_minmax(uint32_t a, uint32_t b, int want_max)
{
    if (is_snan(a) || is_snan(b)) fflags |= FF_NV;
    if (is_nan(a) && is_nan(b)) return CANON_NAN;
    if (is_nan(a)) return b;
    if (is_nan(b)) return a;
    if (!((a | b) & 0x7fffffff))                       /* both zeros: -0 < +0 */
        return want_max ? (a & b) : (a | b);
    float x = b2f(a), y = b2f(b);
    return want_max ? (x > y ? a : b) : (x < y ? a : b);
}

/* f3: 2 = feq (quiet), 1 = flt, 0 = fle (signaling) */
static uint32_t fp_cmp(uint32_t a, uint32_t b, int f3)
{
    if (is_nan(a) || is_nan(b)) {
        if (f3 != 2 || is_snan(a) || is_snan(b)) fflags |= FF_NV;
        return 0;
    }
    float x = b2f(a), y = b2f(b);
    return f3 == 2 ? x == y : f3 == 1 ? x < y : x <= y;
}

static uint32_t fp_class(uint32_t a)
{
    uint32_t sign = a >> 31, e = a >> 23 & 0xff, m = a & 0x7fffff;
    if (e == 0xff) return m ? (m & 0x400000 ? 1u << 9 : 1u << 8) : (sign ? 1u << 0 : 1u << 7);
    if (e == 0) return m ? (sign ? 1u << 2 : 1u << 5) : (sign ? 1u << 3 : 1u << 4);
    return sign ? 1u << 1 : 1u << 6;
}

/* fcvt.w[u].s: round per rm, saturate, NV on NaN / out of range, NX when inexact. */
static uint32_t fp_to_int(uint32_t a, int is_unsigned, int rm)
{
    if (is_nan(a)) { fflags |= FF_NV; return is_unsigned ? 0xffffffffu : 0x7fffffffu; }
    double d = b2f(a), r;
    switch (rm) {
    case RNE: r = nearbyint(d); break;         /* host stays in round-to-nearest-even */
    case RTZ: r = trunc(d); break;
    case RDN: r = floor(d); break;
    case RUP: r = ceil(d); break;
    default:  r = round(d); break;             /* RMM: ties away from zero */
    }
    if (is_unsigned) {
        if (r < 0) { fflags |= FF_NV; return 0; }
        if (r > 4294967295.0) { fflags |= FF_NV; return 0xffffffffu; }
        if (r != d) fflags |= FF_NX;
        return (uint32_t)r;
    }
    if (r < -2147483648.0) { fflags |= FF_NV; return 0x80000000u; }
    if (r > 2147483647.0) { fflags |= FF_NV; return 0x7fffffffu; }
    if (r != d) fflags |= FF_NX;
    return (uint32_t)(int32_t)r;
}

static uint32_t fp_from_int(uint32_t v, int is_unsigned, int rm)
{
    int64_t w = is_unsigned ? (int64_t)v : (int64_t)(int32_t)v;
    if (rm == RMM) return f2b(d2s_rmm((double)w, &fflags));
    return f2b(host_i2s(w, rm, &fflags));
}

/* ------------------------------------------------------------------------------------------ */
/* CSRs                                                                                       */

static int csr_read(uint32_t csr, uint32_t *v)
{
    uint64_t t = instret;
    switch (csr) {
    case 0x001: *v = fflags; return 1;
    case 0x002: *v = frm; return 1;
    case 0x003: *v = frm << 5 | fflags; return 1;
    case 0x300: *v = mstatus; return 1;
    case 0x301: *v = 0x40001129; return 1;                  /* misa: RV32 A C F I M */
    case 0xF14: *v = 0; return 1;                           /* mhartid */
    case 0xC00: case 0xC02: case 0xB00: case 0xB02: *v = (uint32_t)t; return 1;   /* cycle = instret */
    case 0xC80: case 0xC82: case 0xB80: case 0xB82: *v = (uint32_t)(t >> 32); return 1;
    case 0xC01: *v = (uint32_t)(t / 400); return 1;         /* time: 1 us ticks at 400 MIPS */
    case 0xC81: *v = (uint32_t)((t / 400) >> 32); return 1;
    }
    return 0;
}

static int csr_write(uint32_t csr, uint32_t v)
{
    switch (csr) {
    case 0x001: fflags = v & 31; return 1;
    case 0x002: frm = v & 7; return 1;
    case 0x003: fflags = v & 31; frm = v >> 5 & 7; return 1;
    case 0x300: mstatus = v; return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* SYSCALLS: a7 = number, a0..a5 = args, result in a0 (negative newlib errno on failure).     */
/* Constants (open flags, AT_*, struct layouts) are the guest's newlib ones.                  */

enum {
    SYS_GETCWD_UNUSED = 17,
    SYS_MKDIRAT = 34,        /* (dirfd, path, mode) */
    SYS_UNLINKAT = 35,       /* (dirfd, path, flags) flags & AT_REMOVEDIR(8) -> rmdir */
    SYS_RENAMEAT = 38,       /* (olddirfd, old, newdirfd, new) */
    SYS_OPENAT = 56,         /* (dirfd, path, flags, mode) */
    SYS_CLOSE = 57,
    SYS_LSEEK = 62,          /* (fd, offset, whence) -> new offset */
    SYS_READ = 63,
    SYS_WRITE = 64,
    SYS_FSTATAT = 79,        /* (dirfd, path, struct stat *, flags) */
    SYS_FSTAT = 80,          /* (fd, struct stat *) */
    SYS_EXIT = 93,
    SYS_EXIT_GROUP = 94,
    SYS_KILL = 129,          /* (pid, sig) */
    SYS_GETTIMEOFDAY = 169,  /* (struct timeval *) */
    SYS_GETPID = 172,
    SYS_SIM_OPENDIR = 2000,  /* (path) -> handle */
    SYS_SIM_READDIR = 2001,  /* (handle, struct dirent *) -> 1 entry, 0 end */
    SYS_SIM_CLOSEDIR = 2002, /* (handle) */
    SYS_SIM_REWINDDIR = 2003,/* (handle) */
};

/* newlib values */
#define G_O_ACCMODE 3
#define G_O_APPEND 0x0008
#define G_O_CREAT 0x0200
#define G_O_TRUNC 0x0400
#define G_O_EXCL 0x0800
#define G_AT_REMOVEDIR 8
#define G_S_IFCHR 0020000
#define G_S_IFDIR 0040000
#define G_S_IFREG 0100000
/* struct stat (88 bytes): st_dev u16 @0, st_ino u16 @2, st_mode u32 @4, st_nlink u16 @8,
 * st_uid u16 @10, st_gid u16 @12, st_rdev u16 @14, st_size i32 @16, st_atim @24, st_mtim @40,
 * st_ctim @56 (struct timespec {int64 tv_sec; long tv_nsec;} = 16), st_blksize @72, st_blocks @76 */
#define G_STAT_SIZE 88
/* struct dirent (ESP-IDF, 260 bytes): ino_t(u16) d_ino @0, uint8 d_type @2, char d_name[256] @3 */
#define G_DIRENT_SIZE 260
#define G_DT_REG 1
#define G_DT_DIR 2

static char sdroot[1024];
#define MAX_FD 64
static int fdmap[MAX_FD];                 /* guest fd -> host fd, -1 = free */
#define MAX_DIRS 32
static struct { DIR *d; char path[1100]; } dirs[MAX_DIRS];

static int32_t gerr(int e)
{
    switch (e) {
    case ENOENT: return -2;   case EIO: return -5;     case EBADF: return -9;
    case ENOMEM: return -12;  case EACCES: return -13; case EBUSY: return -16;
    case EEXIST: return -17;  case EXDEV: return -18;  case ENOTDIR: return -20;
    case EISDIR: return -21;  case EINVAL: return -22; case ENFILE: return -23;
    case EMFILE: return -24;  case ENOSPC: return -28; case ESPIPE: return -29;
    case EROFS: return -30;   case ENOTEMPTY: return -90; case ENAMETOOLONG: return -91;
    case EPERM: return -1;
    }
    return -5;
}

/* Guest path -> host path. Only /sd and /sd/... exist; ".." components are refused. */
static int32_t map_path(uint32_t gaddr, char *out, size_t n)
{
    const char *g = gstr(gaddr);
    if (strncmp(g, "/sd", 3) || (g[3] && g[3] != '/')) return -2;
    if (strstr(g, "/../") || (strlen(g) >= 3 && !strcmp(g + strlen(g) - 3, "/.."))) return -13;
    if ((size_t)snprintf(out, n, "%s%s", sdroot, g + 3) >= n) return -91;
    size_t l = strlen(out);
    while (l > strlen(sdroot) && (out[l - 1] == '/' || out[l - 1] == '\\')) out[--l] = 0;
    return 0;
}

static void put_stat(uint32_t gaddr, const struct stat *st, uint32_t mode)
{
    uint8_t *p = maddr(gaddr, G_STAT_SIZE, "store");
    memset(p, 0, G_STAT_SIZE);
    put32(p + 4, mode);
    put16(p + 8, 1);
    if (st) {
        put32(p + 16, (uint32_t)st->st_size);
        put64(p + 24, (uint64_t)(int64_t)st->st_atime);
        put64(p + 40, (uint64_t)(int64_t)st->st_mtime);
        put64(p + 56, (uint64_t)(int64_t)st->st_ctime);
        put32(p + 76, (uint32_t)((st->st_size + 511) / 512));
    }
    put32(p + 72, 512);
}

static uint32_t host_mode(const struct stat *st)
{
    return (S_ISDIR(st->st_mode) ? G_S_IFDIR : G_S_IFREG) | 0777;  /* FAT: everything rwx */
}

static int fd_of(uint32_t gfd)
{
    return gfd < MAX_FD ? fdmap[gfd] : -1;
}

static int32_t do_ecall(void)
{
    uint32_t a0 = xr[10], a1 = xr[11], a2 = xr[12], a3 = xr[13];
    char path[1100], path2[1100];
    int32_t r;
    struct stat st;

    switch (xr[17]) {
    case SYS_WRITE: {
        int h = fd_of(a0);
        if (h < 0) return -9;
        if (!a2) return 0;
        uint8_t *p = maddr(a1, a2, "write() buffer");
        if (a0 == 1 || a0 == 2) fflush(stdout);
        r = (int32_t)write(h, p, a2);
        return r < 0 ? gerr(errno) : r;
    }
    case SYS_READ: {
        int h = fd_of(a0);
        if (h < 0) return -9;
        if (!a2) return 0;
        r = (int32_t)read(h, maddr(a1, a2, "read() buffer"), a2);
        return r < 0 ? gerr(errno) : r;
    }
    case SYS_OPENAT: {
        if ((r = map_path(a1, path, sizeof path))) return r;
        int fl = O_BINARY;
        switch (a2 & G_O_ACCMODE) { case 0: fl |= O_RDONLY; break; case 1: fl |= O_WRONLY; break; default: fl |= O_RDWR; }
        if (a2 & G_O_APPEND) fl |= O_APPEND;
        if (a2 & G_O_CREAT) fl |= O_CREAT;
        if (a2 & G_O_TRUNC) fl |= O_TRUNC;
        if (a2 & G_O_EXCL) fl |= O_EXCL;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return -21;
        int g;
        for (g = 3; g < MAX_FD && fdmap[g] >= 0; g++) {}
        if (g == MAX_FD) return -23;
        int h = open(path, fl, 0666);
        if (h < 0) return gerr(errno);
        fdmap[g] = h;
        return g;
    }
    case SYS_CLOSE: {
        int h = fd_of(a0);
        if (h < 0) return -9;
        if (a0 > 2) { close(h); fdmap[a0] = -1; }
        return 0;
    }
    case SYS_LSEEK: {
        int h = fd_of(a0);
        if (h < 0) return -9;
        if (a0 <= 2) return -29;
        long o = lseek(h, (long)(int32_t)a1, (int)a2);
        return o < 0 ? gerr(errno) : (int32_t)o;
    }
    case SYS_FSTAT: {
        int h = fd_of(a0);
        if (h < 0) return -9;
        if (a0 <= 2) { put_stat(a1, NULL, G_S_IFCHR | 0666); return 0; }   /* console */
        if (fstat(h, &st)) return gerr(errno);
        put_stat(a1, &st, host_mode(&st));
        return 0;
    }
    case SYS_FSTATAT:
        if ((r = map_path(a1, path, sizeof path))) return r;
        if (stat(path, &st)) return gerr(errno);
        put_stat(a2, &st, host_mode(&st));
        return 0;
    case SYS_MKDIRAT:
        if ((r = map_path(a1, path, sizeof path))) return r;
        return host_mkdir(path) ? gerr(errno) : 0;
    case SYS_UNLINKAT:
        if ((r = map_path(a1, path, sizeof path))) return r;
        if (a2 & G_AT_REMOVEDIR) return rmdir(path) ? gerr(errno) : 0;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return -21;
        return unlink(path) ? gerr(errno) : 0;
    case SYS_RENAMEAT:
        if ((r = map_path(a1, path, sizeof path)) || (r = map_path(a3, path2, sizeof path2))) return r;
        if (stat(path2, &st) == 0) return -17;      /* FATFS f_rename never replaces */
        return rename(path, path2) ? gerr(errno) : 0;
    case SYS_GETTIMEOFDAY: {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (a0) {
            uint8_t *p = maddr(a0, 16, "store");
            put64(p, (uint64_t)(int64_t)tv.tv_sec);
            put32(p + 8, (uint32_t)tv.tv_usec);
            put32(p + 12, 0);
        }
        return 0;
    }
    case SYS_GETPID:
        return 1;
    case SYS_KILL:
        if (a1 == 0) return 0;
        die(134, "guest killed by signal %u%s", a1, a1 == 6 ? " (SIGABRT, abort())" : "");
        return 0;
    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        fflush(stdout);
        fflush(stderr);
        exit((int)a0);
    case SYS_SIM_OPENDIR: {
        if ((r = map_path(a0, path, sizeof path))) return r;
        int h;
        for (h = 0; h < MAX_DIRS && dirs[h].d; h++) {}
        if (h == MAX_DIRS) return -23;
        if (stat(path, &st)) return gerr(errno);
        if (!S_ISDIR(st.st_mode)) return -20;
        if (!(dirs[h].d = opendir(path))) return gerr(errno);
        snprintf(dirs[h].path, sizeof dirs[h].path, "%s", path);
        return h + 1;
    }
    case SYS_SIM_READDIR: {
        uint32_t h = a0 - 1;
        if (h >= MAX_DIRS || !dirs[h].d) return -9;
        struct dirent *e;
        while ((e = readdir(dirs[h].d)) && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))) {}
        if (!e) return 0;
        uint8_t *p = maddr(a1, G_DIRENT_SIZE, "store");
        memset(p, 0, G_DIRENT_SIZE);
        snprintf(path, sizeof path, "%s/%s", dirs[h].path, e->d_name);
        p[2] = stat(path, &st) ? 0 : S_ISDIR(st.st_mode) ? G_DT_DIR : G_DT_REG;
        size_t nl = strlen(e->d_name);
        memcpy(p + 3, e->d_name, nl > 255 ? 255 : nl);      /* NUL from the memset */
        return 1;
    }
    case SYS_SIM_CLOSEDIR: case SYS_SIM_REWINDDIR: {
        uint32_t h = a0 - 1;
        if (h >= MAX_DIRS || !dirs[h].d) return -9;
        if (xr[17] == SYS_SIM_REWINDDIR) { rewinddir(dirs[h].d); return 0; }
        closedir(dirs[h].d);
        dirs[h].d = NULL;
        return 0;
    }
    }
    die(132, "unknown syscall %u (a7) with a0=0x%x a1=0x%x a2=0x%x", xr[17], a0, a1, a2);
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Interpreter                                                                                */

static void illegal(uint32_t inst, int len)
{
    if (len == 2) die(132, "illegal instruction 0x%04x (compressed)", inst);
    die(132, "illegal instruction 0x%08x", inst);
}

static void run(void)
{
    uint64_t limit = max_insns;
    for (;;) {
        uint32_t inst, npc, raw;
        int len;
        uint16_t lo;
        memcpy(&lo, maddr(pc, 2, "instruction fetch"), 2);
        if ((lo & 3) == 3) {
            memcpy(&inst, maddr(pc, 4, "instruction fetch"), 4);
            raw = inst;
            len = 4;
        } else {
            raw = lo;
            inst = cexp[lo];
            len = 2;
            if (!inst) illegal(raw, 2);
        }
        npc = pc + len;
        if (__builtin_expect(++instret > limit, 0))
            die(124, "instruction limit (%llu) reached", (unsigned long long)limit);
        if (__builtin_expect(trace, 0)) {
            char sb[160];
            fprintf(stderr, "%08x: %0*x  %-28s sp=%08x a0=%08x a1=%08x\n", pc, len * 2, raw,
                    sym_at(pc, sb, sizeof sb), xr[2], xr[10], xr[11]);
        }

        uint32_t rd = inst >> 7 & 31, rs1 = inst >> 15 & 31, rs2 = inst >> 20 & 31, f3 = inst >> 12 & 7;
        int32_t imm_i = (int32_t)inst >> 20;
        switch (inst & 0x7f) {
        case 0x37: xr[rd] = inst & 0xfffff000; break;                    /* lui */
        case 0x17: xr[rd] = pc + (inst & 0xfffff000); break;             /* auipc */
        case 0x6f: {                                                     /* jal */
            int32_t imm = ((int32_t)(inst & 0x80000000) >> 11) | (inst & 0xff000) |
                          (inst >> 9 & 0x800) | (inst >> 20 & 0x7fe);
            xr[rd] = npc;
            npc = pc + imm;
            break;
        }
        case 0x67: {                                                     /* jalr */
            if (f3) illegal(raw, len);
            uint32_t t = (xr[rs1] + imm_i) & ~1u;
            xr[rd] = npc;
            npc = t;
            break;
        }
        case 0x63: {                                                     /* branches */
            int32_t imm = ((int32_t)(inst & 0x80000000) >> 19) | (inst << 4 & 0x800) |
                          (inst >> 20 & 0x7e0) | (inst >> 7 & 0x1e);
            uint32_t a = xr[rs1], b = xr[rs2];
            int take;
            switch (f3) {
            case 0: take = a == b; break;
            case 1: take = a != b; break;
            case 4: take = (int32_t)a < (int32_t)b; break;
            case 5: take = (int32_t)a >= (int32_t)b; break;
            case 6: take = a < b; break;
            case 7: take = a >= b; break;
            default: illegal(raw, len); take = 0;
            }
            if (take) npc = pc + imm;
            break;
        }
        case 0x03: {                                                     /* loads */
            uint32_t a = xr[rs1] + imm_i, v;
            switch (f3) {
            case 0: v = (uint32_t)(int8_t)ld8(a); break;
            case 1: v = (uint32_t)(int16_t)ld16(a); break;
            case 2: v = ld32(a); break;
            case 4: v = ld8(a); break;
            case 5: v = ld16(a); break;
            default: illegal(raw, len); v = 0;
            }
            xr[rd] = v;
            break;
        }
        case 0x23: {                                                     /* stores */
            uint32_t a = xr[rs1] + (uint32_t)(((int32_t)inst >> 25 << 5) | (inst >> 7 & 31));
            switch (f3) {
            case 0: st8(a, xr[rs2]); break;
            case 1: st16(a, xr[rs2]); break;
            case 2: st32(a, xr[rs2]); break;
            default: illegal(raw, len);
            }
            break;
        }
        case 0x13: {                                                     /* op-imm */
            uint32_t a = xr[rs1], f7 = inst >> 25;
            switch (f3) {
            case 0: xr[rd] = a + imm_i; break;
            case 2: xr[rd] = (int32_t)a < imm_i; break;
            case 3: xr[rd] = a < (uint32_t)imm_i; break;
            case 4: xr[rd] = a ^ imm_i; break;
            case 6: xr[rd] = a | imm_i; break;
            case 7: xr[rd] = a & imm_i; break;
            case 1: if (f7) illegal(raw, len); xr[rd] = a << rs2; break;
            default:
                if (f7 == 0) xr[rd] = a >> rs2;
                else if (f7 == 0x20) xr[rd] = (uint32_t)((int32_t)a >> rs2);
                else illegal(raw, len);
            }
            break;
        }
        case 0x33: {                                                     /* op, M extension */
            uint32_t a = xr[rs1], b = xr[rs2], v;
            switch ((inst >> 25) << 3 | f3) {
            case 0x000: v = a + b; break;
            case 0x100: v = a - b; break;
            case 0x001: v = a << (b & 31); break;
            case 0x002: v = (int32_t)a < (int32_t)b; break;
            case 0x003: v = a < b; break;
            case 0x004: v = a ^ b; break;
            case 0x005: v = a >> (b & 31); break;
            case 0x105: v = (uint32_t)((int32_t)a >> (b & 31)); break;
            case 0x006: v = a | b; break;
            case 0x007: v = a & b; break;
            case 0x008: v = a * b; break;
            case 0x009: v = (uint32_t)((uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32); break;
            case 0x00a: v = (uint32_t)((uint64_t)((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32); break;
            case 0x00b: v = (uint32_t)(((uint64_t)a * b) >> 32); break;
            case 0x00c:                                                  /* div */
                if (b == 0) v = 0xffffffffu;
                else if (a == 0x80000000u && b == 0xffffffffu) v = a;
                else v = (uint32_t)((int32_t)a / (int32_t)b);
                break;
            case 0x00d: v = b ? a / b : 0xffffffffu; break;              /* divu */
            case 0x00e:                                                  /* rem */
                if (b == 0) v = a;
                else if (a == 0x80000000u && b == 0xffffffffu) v = 0;
                else v = (uint32_t)((int32_t)a % (int32_t)b);
                break;
            case 0x00f: v = b ? a % b : a; break;                        /* remu */
            default: illegal(raw, len); v = 0;
            }
            xr[rd] = v;
            break;
        }
        case 0x0f: break;                                                /* fence, fence.i */
        case 0x73: {                                                     /* system */
            if (f3 == 0) {
                if (inst == 0x00000073) xr[10] = (uint32_t)do_ecall();
                else if (inst == 0x00100073) die(133, "ebreak");
                else if (inst == 0x10500073) {}                          /* wfi */
                else illegal(raw, len);
                break;
            }
            if (f3 == 4) illegal(raw, len);
            uint32_t csr = inst >> 20, old, src = (f3 & 4) ? rs1 : xr[rs1], nv;
            if (!csr_read(csr, &old)) illegal(raw, len);
            switch (f3 & 3) {
            case 1: nv = src; break;
            case 2: nv = old | src; break;
            default: nv = old & ~src; break;
            }
            if (((f3 & 3) == 1 || rs1 != 0) && !csr_write(csr, nv)) illegal(raw, len);
            xr[rd] = old;
            break;
        }
        case 0x2f: {                                                     /* A extension */
            if (f3 != 2) illegal(raw, len);
            uint32_t a = xr[rs1], b = xr[rs2], v;
            if (a & 3) die(139, "misaligned atomic access at 0x%08x", a);
            switch (inst >> 27) {
            case 0x02: if (rs2) illegal(raw, len); xr[rd] = ld32(a); lr_addr = a; lr_valid = 1; break;
            case 0x03:                                                   /* sc.w */
                if (lr_valid && lr_addr == a) { st32(a, b); xr[rd] = 0; } else xr[rd] = 1;
                lr_valid = 0;
                break;
            default:
                v = ld32(a);
                switch (inst >> 27) {
                case 0x01: st32(a, b); break;
                case 0x00: st32(a, v + b); break;
                case 0x04: st32(a, v ^ b); break;
                case 0x0c: st32(a, v & b); break;
                case 0x08: st32(a, v | b); break;
                case 0x10: st32(a, (int32_t)v < (int32_t)b ? v : b); break;
                case 0x14: st32(a, (int32_t)v > (int32_t)b ? v : b); break;
                case 0x18: st32(a, v < b ? v : b); break;
                case 0x1c: st32(a, v > b ? v : b); break;
                default: illegal(raw, len);
                }
                xr[rd] = v;
            }
            break;
        }
        case 0x07:                                                       /* flw */
            if (f3 != 2) illegal(raw, len);
            fr[rd] = ld32(xr[rs1] + imm_i);
            break;
        case 0x27:                                                       /* fsw */
            if (f3 != 2) illegal(raw, len);
            st32(xr[rs1] + (uint32_t)(((int32_t)inst >> 25 << 5) | (inst >> 7 & 31)), fr[rs2]);
            break;
        case 0x43: case 0x47: case 0x4b: case 0x4f: {                    /* fmadd fmsub fnmsub fnmadd */
            int rm = f3 == 7 ? (int)frm : (int)f3;
            if ((inst >> 25 & 3) || rm > 4) illegal(raw, len);
            int op = inst >> 2 & 3;                                      /* 0 madd 1 msub 2 nmsub 3 nmadd */
            fr[rd] = fp_fma(fr[rs1], fr[rs2], fr[inst >> 27], op >= 2, op == 1 || op == 3, rm);
            break;
        }
        case 0x53: {                                                     /* op-fp, single only */
            uint32_t f7 = inst >> 25, a = fr[rs1], b = fr[rs2];
            int rm = f3 == 7 ? (int)frm : (int)f3;
            switch (f7) {
            case 0x00: case 0x04: case 0x08: case 0x0c:
                if (rm > 4) illegal(raw, len);
                fr[rd] = fp_arith(f7 >> 2, a, b, rm);
                break;
            case 0x2c:
                if (rm > 4 || rs2) illegal(raw, len);
                fr[rd] = fp_arith(F_SQRT, a, a, rm);
                break;
            case 0x10:
                if (f3 == 0) fr[rd] = (a & 0x7fffffff) | (b & 0x80000000);
                else if (f3 == 1) fr[rd] = (a & 0x7fffffff) | (~b & 0x80000000);
                else if (f3 == 2) fr[rd] = a ^ (b & 0x80000000);
                else illegal(raw, len);
                break;
            case 0x14:
                if (f3 > 1) illegal(raw, len);
                fr[rd] = fp_minmax(a, b, f3);
                break;
            case 0x50:
                if (f3 > 2) illegal(raw, len);
                xr[rd] = fp_cmp(a, b, f3);
                break;
            case 0x60:
                if (rm > 4 || rs2 > 1) illegal(raw, len);
                xr[rd] = fp_to_int(a, rs2, rm);
                break;
            case 0x68:
                if (rm > 4 || rs2 > 1) illegal(raw, len);
                fr[rd] = fp_from_int(xr[rs1], rs2, rm);
                break;
            case 0x70:
                if (rs2) illegal(raw, len);
                if (f3 == 0) xr[rd] = a;
                else if (f3 == 1) xr[rd] = fp_class(a);
                else illegal(raw, len);
                break;
            case 0x78:
                if (rs2 || f3) illegal(raw, len);
                fr[rd] = xr[rs1];
                break;
            default:
                illegal(raw, len);
            }
            break;
        }
        default:
            illegal(raw, len);
        }
        xr[0] = 0;
        pc = npc;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* ELF loading and startup                                                                    */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

static uint32_t load_elf(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "rvsim: cannot open %s\n", path); exit(125); }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *e = elf_image = malloc((size_t)n);
    if (!e || fread(e, 1, (size_t)n, fp) != (size_t)n) { fprintf(stderr, "rvsim: read error\n"); exit(125); }
    fclose(fp);
    if (n < 52 || memcmp(e, "\x7f" "ELF", 4) || e[4] != 1 || e[5] != 1 || rd16(e + 18) != 243) {
        fprintf(stderr, "rvsim: %s is not a little-endian ELF32 RISC-V file\n", path);
        exit(125);
    }
    uint32_t phoff = rd32(e + 28), shoff = rd32(e + 32);
    uint32_t phent = rd16(e + 42), phnum = rd16(e + 44), shent = rd16(e + 46), shnum = rd16(e + 48);
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = e + phoff + i * phent;
        if (rd32(ph) != 1) continue;                                     /* PT_LOAD */
        uint32_t off = rd32(ph + 4), va = rd32(ph + 8), fsz = rd32(ph + 16), msz = rd32(ph + 20);
        if (!msz) continue;
        if (va - MEM_BASE > mem_size || msz > mem_size - (va - MEM_BASE) || fsz > msz ||
            off + (uint64_t)fsz > (uint64_t)n) {
            fprintf(stderr, "rvsim: segment 0x%08x+0x%x does not fit RAM 0x%08x+0x%x\n", va, msz,
                    MEM_BASE, mem_size);
            exit(125);
        }
        memcpy(mem + (va - MEM_BASE), e + off, fsz);
        memset(mem + (va - MEM_BASE) + fsz, 0, msz - fsz);
    }
    /* symbol table for diagnostics */
    for (uint32_t i = 0; shoff && i < shnum; i++) {
        const uint8_t *sh = e + shoff + i * shent;
        if (rd32(sh + 4) != 2) continue;                                 /* SHT_SYMTAB */
        const uint8_t *strsh = e + shoff + rd32(sh + 24) * shent;
        const char *strtab = (const char *)e + rd32(strsh + 16);
        uint32_t off = rd32(sh + 16), cnt = rd32(sh + 20) / 16;
        syms = calloc(cnt, sizeof *syms);
        for (uint32_t k = 0; k < cnt; k++) {
            const uint8_t *s = e + off + k * 16;
            int type = s[12] & 15;
            if ((type == 1 || type == 2) && rd32(s) && rd16(s + 14)) {   /* OBJECT/FUNC, defined */
                syms[nsyms].addr = rd32(s + 4);
                syms[nsyms].name = strtab + rd32(s);
                nsyms++;
            }
        }
        qsort(syms, (size_t)nsyms, sizeof *syms, sym_cmp);
        break;
    }
    return rd32(e + 24);
}

/* argv strings and the argv[] array at the top of RAM; sp below them, 16-byte aligned. */
static void setup_args(int argc, char **argv)
{
    uint32_t top = MEM_BASE + mem_size, sp = top;
    uint32_t *ptrs = calloc((size_t)argc + 1, 4);
    for (int i = argc - 1; i >= 0; i--) {
        size_t l = strlen(argv[i]) + 1;
        sp -= (uint32_t)l;
        memcpy(maddr(sp, (uint32_t)l, "setup"), argv[i], l);
        ptrs[i] = sp;
    }
    sp = (sp - 4 * (uint32_t)(argc + 1)) & ~15u;
    for (int i = 0; i <= argc; i++) st32(sp + 4 * i, ptrs[i]);
    xr[10] = (uint32_t)argc;
    xr[11] = sp;
    xr[2] = (sp - 16) & ~15u;
    free(ptrs);
}

static void mkdirs(const char *p)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", p);
    for (char *s = buf + 1; *s; s++)
        if ((*s == '/' || *s == '\\') && s[-1] != ':') { char c = *s; *s = 0; host_mkdir(buf); *s = c; }
    host_mkdir(buf);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: rvsim [--trace] [--sd DIR] [--max-insns N] [--mem MB] [--stats] prog.elf [args...]\n"
            "  --sd DIR       host directory backing guest paths /sd/... (default: <tmp>/rvsim_sd)\n"
            "  --max-insns N  stop after N instructions (default 20e9)\n"
            "  --mem MB       guest RAM size at 0x40000000 (default 64)\n"
            "  --trace        print every executed instruction to stderr\n"
            "  --stats        print instruction count and speed to stderr at exit\n");
    exit(125);
}

static int stats;
static struct timeval t_start;

static void print_stats(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    double s = (double)(t.tv_sec - t_start.tv_sec) + (t.tv_usec - t_start.tv_usec) * 1e-6;
    fprintf(stderr, "rvsim: %llu instructions in %.3f s (%.1f MIPS)\n", (unsigned long long)instret, s,
            s > 0 ? (double)instret / s / 1e6 : 0.0);
}

int main(int argc, char **argv)
{
    const char *sd = NULL;
    int i;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--stats")) stats = 1;
        else if (!strcmp(argv[i], "--sd") && i + 1 < argc) sd = argv[++i];
        else if (!strcmp(argv[i], "--max-insns") && i + 1 < argc) max_insns = (uint64_t)strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--mem") && i + 1 < argc) mem_size = (uint32_t)atoi(argv[++i]) << 20;
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else usage();
    }
    if (i >= argc || mem_size < (1u << 20) || mem_size > (1024u << 20)) usage();

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    if (sd) snprintf(sdroot, sizeof sdroot, "%s", sd);
    else {
        const char *t = getenv("TMPDIR");
        if (!t) t = getenv("TEMP");
        if (!t) t = getenv("TMP");
        snprintf(sdroot, sizeof sdroot, "%s/rvsim_sd", t ? t : "/tmp");
    }
    for (char *s = sdroot; *s; s++) if (*s == '\\') *s = '/';
    size_t l = strlen(sdroot);
    while (l > 1 && sdroot[l - 1] == '/') sdroot[--l] = 0;
    mkdirs(sdroot);

    for (int k = 0; k < MAX_FD; k++) fdmap[k] = k < 3 ? k : -1;
    for (uint32_t h = 0; h < 65536; h++) cexp[h] = (h & 3) == 3 ? 0 : expand_c(h);
    fp_host_init();

    mem = calloc(1, mem_size);
    if (!mem) { fprintf(stderr, "rvsim: out of memory\n"); return 125; }
    pc = load_elf(argv[i]);
    setup_args(argc - i, argv + i);
    if (stats) { gettimeofday(&t_start, NULL); atexit(print_stats); }
    run();
    return 0;
}
