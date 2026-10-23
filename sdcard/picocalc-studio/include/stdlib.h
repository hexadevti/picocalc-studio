/* stdlib.h - PicoCalc C library (the firmware's newlib; only exported functions declared) */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7fffffff

/* memory */
void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t size);
void free(void *p);

/* conversions */
int atoi(const char *s);
long atol(const char *s);
double atof(const char *s);
long strtol(const char *restrict s, char **restrict end, int base);
unsigned long strtoul(const char *restrict s, char **restrict end, int base);
double strtod(const char *restrict s, char **restrict end);
float strtof(const char *restrict s, char **restrict end);

/* algorithms */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));
int abs(int x);
long labs(long x);
int rand(void);
void srand(unsigned seed);

/* program termination */
_Noreturn void abort(void);
_Noreturn void exit(int status);

#endif
