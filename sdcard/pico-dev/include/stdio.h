/* stdio.h - PicoCalc C library (the firmware's newlib stdio; only exported functions declared) */
#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <sys/reent.h>

typedef struct __sFILE FILE;    /* opaque: use FILE * only */
typedef long fpos_t;

#define EOF          (-1)
#define BUFSIZ       128
#define FOPEN_MAX    20
#define FILENAME_MAX 1024
#define L_tmpnam     FILENAME_MAX
#define TMP_MAX      26
#define SEEK_SET     0
#define SEEK_CUR     1
#define SEEK_END     2
#define _IOFBF       0
#define _IOLBF       1
#define _IONBF       2

#define stdin  (_REENT->_stdin)
#define stdout (_REENT->_stdout)
#define stderr (_REENT->_stderr)

/* formatted output */
int printf(const char *restrict fmt, ...);
int fprintf(FILE *restrict f, const char *restrict fmt, ...);
int sprintf(char *restrict buf, const char *restrict fmt, ...);
int snprintf(char *restrict buf, size_t n, const char *restrict fmt, ...);
int vprintf(const char *restrict fmt, va_list ap);
int vsnprintf(char *restrict buf, size_t n, const char *restrict fmt, va_list ap);

/* formatted input */
int fscanf(FILE *restrict f, const char *restrict fmt, ...);
int sscanf(const char *restrict s, const char *restrict fmt, ...);

/* character and line I/O */
int fgetc(FILE *f);
int fputc(int c, FILE *f);
char *fgets(char *restrict s, int n, FILE *restrict f);
int fputs(const char *restrict s, FILE *restrict f);
int getchar(void);
int putchar(int c);
int puts(const char *s);
#define getc(f)    fgetc(f)
#define putc(c, f) fputc((c), (f))

/* files */
FILE *fopen(const char *restrict path, const char *restrict mode);
int fclose(FILE *f);
size_t fread(void *restrict p, size_t size, size_t n, FILE *restrict f);
size_t fwrite(const void *restrict p, size_t size, size_t n, FILE *restrict f);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int fflush(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
int remove(const char *path);
int rename(const char *from, const char *to);

#endif
