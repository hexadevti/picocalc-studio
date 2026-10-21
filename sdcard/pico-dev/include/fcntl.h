/* fcntl.h - PicoCalc C library (flag values match the firmware's newlib)
 * NOTE: open() is not exported by the firmware yet; use fopen(). */
#ifndef _FCNTL_H
#define _FCNTL_H

#include <sys/types.h>

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   (O_RDONLY | O_WRONLY | O_RDWR)
#define O_APPEND    0x0008
#define O_CREAT     0x0200
#define O_TRUNC     0x0400
#define O_EXCL      0x0800
#define O_SYNC      0x2000
#define O_NONBLOCK  0x4000
#define O_NOCTTY    0x8000
#define O_CLOEXEC   0x40000
#define O_NOFOLLOW  0x100000
#define O_DIRECTORY 0x200000

#define F_DUPFD    0
#define F_GETFD    1
#define F_SETFD    2
#define F_GETFL    3
#define F_SETFL    4
#define FD_CLOEXEC 1

int open(const char *path, int flags, ...);

#endif
