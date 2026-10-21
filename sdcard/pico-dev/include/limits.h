/* limits.h - PicoCalc C library (RV32 ilp32f: plain char is unsigned) */
#ifndef _LIMITS_H
#define _LIMITS_H

#define CHAR_BIT   8
#define SCHAR_MIN  (-0x7f - 1)
#define SCHAR_MAX  0x7f
#define UCHAR_MAX  0xff
#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN   0
#define CHAR_MAX   UCHAR_MAX
#else
#define CHAR_MIN   SCHAR_MIN
#define CHAR_MAX   SCHAR_MAX
#endif
#define MB_LEN_MAX 1
#define SHRT_MIN   (-0x7fff - 1)
#define SHRT_MAX   0x7fff
#define USHRT_MAX  0xffff
#define INT_MIN    (-0x7fffffff - 1)
#define INT_MAX    0x7fffffff
#define UINT_MAX   0xffffffffU
#define LONG_MIN   (-0x7fffffffL - 1L)
#define LONG_MAX   0x7fffffffL
#define ULONG_MAX  0xffffffffUL
#define LLONG_MIN  (-0x7fffffffffffffffLL - 1LL)
#define LLONG_MAX  0x7fffffffffffffffLL
#define ULLONG_MAX 0xffffffffffffffffULL

#define PATH_MAX   1024
#define NAME_MAX   255

#endif
