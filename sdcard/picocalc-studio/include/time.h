/* time.h - PicoCalc C library (types match the firmware's newlib: time_t is 64-bit)
 * NOTE: the time functions are not exported by the firmware yet; calling them fails to link. */
#ifndef _TIME_H
#define _TIME_H

#include <stddef.h>

#ifndef _TIME_T_DECLARED
#define _TIME_T_DECLARED
typedef long long time_t;
#endif
#ifndef _CLOCK_T_DECLARED
#define _CLOCK_T_DECLARED
typedef unsigned long clock_t;
#endif

#define CLOCKS_PER_SEC 1000000

#ifndef _STRUCT_TIMESPEC_DECLARED
#define _STRUCT_TIMESPEC_DECLARED
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};
#endif

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

clock_t clock(void);
time_t time(time_t *t);
double difftime(time_t a, time_t b);
time_t mktime(struct tm *tm);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
size_t strftime(char *restrict s, size_t n, const char *restrict fmt, const struct tm *restrict tm);

#endif
