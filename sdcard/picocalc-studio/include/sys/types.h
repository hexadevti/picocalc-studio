/* sys/types.h - PicoCalc C library (types match the firmware's newlib) */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stddef.h>

typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;
typedef unsigned short ushort;
typedef unsigned int   uint;
typedef unsigned long  ulong;

typedef short          dev_t;
typedef unsigned short ino_t;
typedef unsigned long  mode_t;
typedef unsigned short nlink_t;
typedef unsigned short uid_t;
typedef unsigned short gid_t;
typedef int            pid_t;
typedef long           off_t;
typedef long           blksize_t;
typedef long           blkcnt_t;
typedef long           key_t;
typedef long           suseconds_t;
typedef unsigned long  useconds_t;
typedef unsigned long  clockid_t;
typedef unsigned long  timer_t;
typedef unsigned long  id_t;
typedef char *         caddr_t;
typedef long           daddr_t;
typedef unsigned long long fsblkcnt_t;
typedef unsigned long  fsfilcnt_t;

#ifndef _SSIZE_T_DECLARED
#define _SSIZE_T_DECLARED
typedef int ssize_t;
#endif
#ifndef _TIME_T_DECLARED
#define _TIME_T_DECLARED
typedef long long time_t;
#endif
#ifndef _CLOCK_T_DECLARED
#define _CLOCK_T_DECLARED
typedef unsigned long clock_t;
#endif

#endif
