/* stdarg.h - PicoCalc C library
 *
 * The compiler spills every variadic argument contiguously on the stack (4-byte slots, 8-byte
 * values 8-aligned, structs larger than 8 bytes passed by reference) and provides the hidden
 * local __va_area__ at the first unnamed argument. va_list is a plain pointer, which is
 * ABI-compatible with the firmware's (gcc) va_list, so it can be passed to vprintf() etc. */
#ifndef _STDARG_H
#define _STDARG_H

#if defined(__GNUC__) && !defined(__chibicc__)
/* host-side checking with gcc (tools/host/check_headers.py) */
typedef __builtin_va_list va_list;
typedef __builtin_va_list __gnuc_va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(d, s) __builtin_va_copy(d, s)
#define va_arg(ap, ty) __builtin_va_arg(ap, ty)
#else
typedef char *va_list;
typedef char *__gnuc_va_list;

#define va_start(ap, last) ((ap) = (char *)__va_area__)
#define va_end(ap) ((void)0)
#define va_copy(d, s) ((d) = (s))
#define __va_copy(d, s) ((d) = (s))
#define __va_align(ty) (_Alignof(ty) >= 8 && sizeof(ty) <= 8 ? 8 : 4)
#define __va_slot(ty) (sizeof(ty) > 8 ? 4 : (sizeof(ty) + 3) & ~3)
#define va_arg(ap, ty) ((ap) = (char *)(((unsigned)(ap) + __va_align(ty) - 1) & ~(__va_align(ty) - 1)) + __va_slot(ty), *(ty *)(sizeof(ty) > 8 ? *(char **)((ap) - 4) : (ap) - __va_slot(ty)))
#endif

#endif
