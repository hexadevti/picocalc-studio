// string_align.cpp - strcmp, strncmp, strcpy, strncpy and memcmp that respect alignment.
//
// The RISC-V newlib of arduino-pico's toolchain (pqt-gcc-riscv 5.0.0) builds these five as
// word-at-a-time loops with no alignment test: it assumes misaligned loads and stores work, and
// on the RP2350's Hazard3 cores they trap. SdFat's strncmp(pbs->oemName, "EXFAT", 5), at offset 3
// of a sector buffer, crashed every boot, and memcmp did in FatFile::open. Defining them here
// keeps the linker from pulling newlib's copies, for the firmware and for compiled programs
// (rt/exports.txt) alike.
#if defined(ARDUINO_ARCH_RP2040) && defined(__riscv)
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline bool aligned2(const void *a, const void *b) { return (((uintptr_t)a | (uintptr_t)b) & 3) == 0; }
static inline bool hasZero(uint32_t w) { return ((w - 0x01010101u) & ~w & 0x80808080u) != 0; }

extern "C" {

int strcmp(const char *a, const char *b) {
  if (aligned2(a, b)) {
    const uint32_t *wa = (const uint32_t *)a, *wb = (const uint32_t *)b;
    while (*wa == *wb && !hasZero(*wa)) wa++, wb++;
    a = (const char *)wa;
    b = (const char *)wb;
  }
  while (*a && *a == *b) a++, b++;
  return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  if (aligned2(a, b)) {
    const uint32_t *wa = (const uint32_t *)a, *wb = (const uint32_t *)b;
    while (n >= 4 && *wa == *wb && !hasZero(*wa)) wa++, wb++, n -= 4;
    a = (const char *)wa;
    b = (const char *)wb;
  }
  for (; n; n--, a++, b++)
    if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
  return 0;
}

char *strcpy(char *dst, const char *src) {
  char *d = dst;
  if (aligned2(d, src)) {
    uint32_t *wd = (uint32_t *)d;
    const uint32_t *ws = (const uint32_t *)src;
    while (!hasZero(*ws)) *wd++ = *ws++;
    d = (char *)wd;
    src = (const char *)ws;
  }
  while ((*d++ = *src++)) {}
  return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
  char *d = dst;
  if (aligned2(d, src)) {
    uint32_t *wd = (uint32_t *)d;
    const uint32_t *ws = (const uint32_t *)src;
    while (n >= 4 && !hasZero(*ws)) *wd++ = *ws++, n -= 4;
    d = (char *)wd;
    src = (const char *)ws;
  }
  for (; n && *src; n--) *d++ = *src++;
  if (n) memset(d, 0, n);
  return dst;
}

int memcmp(const void *pa, const void *pb, size_t n) {
  const unsigned char *a = (const unsigned char *)pa, *b = (const unsigned char *)pb;
  if (aligned2(a, b)) {
    const uint32_t *wa = (const uint32_t *)a, *wb = (const uint32_t *)b;
    while (n >= 4 && *wa == *wb) wa++, wb++, n -= 4;
    a = (const unsigned char *)wa;
    b = (const unsigned char *)wb;
  }
  for (; n; n--, a++, b++)
    if (*a != *b) return *a - *b;
  return 0;
}
}
#endif
