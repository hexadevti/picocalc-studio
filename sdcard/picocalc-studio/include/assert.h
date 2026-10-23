/* assert.h - PicoCalc C library (no include guard: NDEBUG may change between inclusions) */
#undef assert
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
int printf(const char *restrict fmt, ...);
_Noreturn void abort(void);
#define assert(e) \
    ((e) ? (void)0 : (printf("%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #e), abort()))
#endif
