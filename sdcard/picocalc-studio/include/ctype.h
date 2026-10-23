/* ctype.h - PicoCalc C library
 *
 * Plain function calls into the firmware's newlib (its table macros are not usable here).
 * iscntrl/isgraph/isblank/isascii/toascii are not exported and are implemented inline (ASCII). */
#ifndef _CTYPE_H
#define _CTYPE_H

int isalpha(int c);
int isalnum(int c);
int isupper(int c);
int islower(int c);
int isspace(int c);
int isdigit(int c);
int isxdigit(int c);
int ispunct(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);

static inline int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7f; }
static inline int isgraph(int c) { return c > 0x20 && c < 0x7f; }
static inline int isblank(int c) { return c == ' ' || c == '\t'; }
static inline int isascii(int c) { return c >= 0 && c < 0x80; }
static inline int toascii(int c) { return c & 0x7f; }

#endif
