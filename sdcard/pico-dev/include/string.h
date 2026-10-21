/* string.h - PicoCalc C library (the firmware's newlib; only exported functions declared) */
#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

void *memset(void *p, int c, size_t n);
void *memcpy(void *restrict d, const void *restrict s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *p, int c, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *restrict d, const char *restrict s);
char *strncpy(char *restrict d, const char *restrict s, size_t n);
char *strcat(char *restrict d, const char *restrict s);
char *strncat(char *restrict d, const char *restrict s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *s, const char *sub);
size_t strcspn(const char *s, const char *reject);
size_t strspn(const char *s, const char *accept);
char *strtok(char *restrict s, const char *restrict delim);
char *strdup(const char *s);

/* also in <strings.h> */
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#endif
