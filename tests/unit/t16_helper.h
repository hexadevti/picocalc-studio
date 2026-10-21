/* helper header for t16_preproc.c - tests #include "local" and include guards (included twice). */
#ifndef T16_HELPER_H
#define T16_HELPER_H

#define HELPER_MAGIC 0x2A

static inline int helper_twice(int x) { return x * 2; }

#endif
