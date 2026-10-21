/* setjmp.h - PicoCalc C library (jmp_buf matches the firmware's newlib: 304 bytes, 8-aligned) */
#ifndef _SETJMP_H
#define _SETJMP_H

typedef long long jmp_buf[38];

int setjmp(jmp_buf env);
_Noreturn void longjmp(jmp_buf env, int val);

#endif
