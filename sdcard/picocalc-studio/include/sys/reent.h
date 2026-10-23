/* sys/reent.h - PicoCalc C library
 *
 * The firmware's newlib keeps per-task C library state in struct _reent (240 bytes,
 * _REENT_SMALL). Only the leading fields are visible here; the rest is padding. */
#ifndef _SYS_REENT_H
#define _SYS_REENT_H

struct __sFILE;

struct _reent {
    int _errno;
    struct __sFILE *_stdin;
    struct __sFILE *_stdout;
    struct __sFILE *_stderr;
    char __reserved[224];
};

struct _reent *__getreent(void);
#define _REENT (__getreent())

#endif
