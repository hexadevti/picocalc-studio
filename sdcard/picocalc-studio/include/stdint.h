/* stdint.h - PicoCalc C library (types match the firmware's newlib / riscv32-esp-elf-gcc:
 * int32_t is `long`, so use PRId32 etc. from <inttypes.h> when printing them) */
#ifndef _STDINT_H
#define _STDINT_H

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef long               int32_t;
typedef unsigned long      uint32_t;
typedef long long          int64_t;
typedef unsigned long long uint64_t;

typedef signed char        int_least8_t;
typedef unsigned char      uint_least8_t;
typedef short              int_least16_t;
typedef unsigned short     uint_least16_t;
typedef long               int_least32_t;
typedef unsigned long      uint_least32_t;
typedef long long          int_least64_t;
typedef unsigned long long uint_least64_t;

typedef int                int_fast8_t;
typedef unsigned int       uint_fast8_t;
typedef int                int_fast16_t;
typedef unsigned int       uint_fast16_t;
typedef int                int_fast32_t;
typedef unsigned int       uint_fast32_t;
typedef long long          int_fast64_t;
typedef unsigned long long uint_fast64_t;

typedef int                intptr_t;
typedef unsigned int       uintptr_t;
typedef long long          intmax_t;
typedef unsigned long long uintmax_t;

#define INT8_MAX   0x7f
#define INT8_MIN   (-0x7f - 1)
#define UINT8_MAX  0xff
#define INT16_MAX  0x7fff
#define INT16_MIN  (-0x7fff - 1)
#define UINT16_MAX 0xffff
#define INT32_MAX  0x7fffffffL
#define INT32_MIN  (-0x7fffffffL - 1)
#define UINT32_MAX 0xffffffffUL
#define INT64_MAX  0x7fffffffffffffffLL
#define INT64_MIN  (-0x7fffffffffffffffLL - 1)
#define UINT64_MAX 0xffffffffffffffffULL

#define INT_LEAST8_MAX   INT8_MAX
#define INT_LEAST8_MIN   INT8_MIN
#define UINT_LEAST8_MAX  UINT8_MAX
#define INT_LEAST16_MAX  INT16_MAX
#define INT_LEAST16_MIN  INT16_MIN
#define UINT_LEAST16_MAX UINT16_MAX
#define INT_LEAST32_MAX  INT32_MAX
#define INT_LEAST32_MIN  INT32_MIN
#define UINT_LEAST32_MAX UINT32_MAX
#define INT_LEAST64_MAX  INT64_MAX
#define INT_LEAST64_MIN  INT64_MIN
#define UINT_LEAST64_MAX UINT64_MAX

#define INT_FAST8_MAX   0x7fffffff
#define INT_FAST8_MIN   (-0x7fffffff - 1)
#define UINT_FAST8_MAX  0xffffffffU
#define INT_FAST16_MAX  0x7fffffff
#define INT_FAST16_MIN  (-0x7fffffff - 1)
#define UINT_FAST16_MAX 0xffffffffU
#define INT_FAST32_MAX  0x7fffffff
#define INT_FAST32_MIN  (-0x7fffffff - 1)
#define UINT_FAST32_MAX 0xffffffffU
#define INT_FAST64_MAX  INT64_MAX
#define INT_FAST64_MIN  INT64_MIN
#define UINT_FAST64_MAX UINT64_MAX

#define INTPTR_MAX  0x7fffffff
#define INTPTR_MIN  (-0x7fffffff - 1)
#define UINTPTR_MAX 0xffffffffU
#define INTMAX_MAX  INT64_MAX
#define INTMAX_MIN  INT64_MIN
#define UINTMAX_MAX UINT64_MAX

#define PTRDIFF_MAX    0x7fffffff
#define PTRDIFF_MIN    (-0x7fffffff - 1)
#define SIZE_MAX       0xffffffffU
#define SIG_ATOMIC_MAX 0x7fffffff
#define SIG_ATOMIC_MIN (-0x7fffffff - 1)
#define WCHAR_MAX      0x7fffffff
#define WCHAR_MIN      (-0x7fffffff - 1)
#define WINT_MAX       0xffffffffU
#define WINT_MIN       0U

#define INT8_C(x)    x
#define UINT8_C(x)   x
#define INT16_C(x)   x
#define UINT16_C(x)  x
#define INT32_C(x)   x##L
#define UINT32_C(x)  x##UL
#define INT64_C(x)   x##LL
#define UINT64_C(x)  x##ULL
#define INTMAX_C(x)  x##LL
#define UINTMAX_C(x) x##ULL

#endif
