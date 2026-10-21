/* stddef.h - PicoCalc C library (layout-compatible with the firmware's newlib, RV32 ilp32f) */
#ifndef _STDDEF_H
#define _STDDEF_H

#ifndef _SIZE_T_DECLARED
#define _SIZE_T_DECLARED
typedef unsigned int size_t;
#endif

#ifndef _PTRDIFF_T_DECLARED
#define _PTRDIFF_T_DECLARED
typedef int ptrdiff_t;
#endif

#ifndef _WCHAR_T_DECLARED
#define _WCHAR_T_DECLARED
typedef int wchar_t;
#endif

typedef struct {
    long long __max_align_ll;
    double __max_align_d;
} max_align_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) ((size_t)&((type *)0)->member)

#endif
