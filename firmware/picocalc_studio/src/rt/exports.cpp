// exports.cpp - lookup in the generated export table.
#include <string.h>
#include "exports.h"

void *rt_find_export(const char *name) {
  uint32_t lo = 0, hi = rt_exports_count;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    const int c = strcmp(name, rt_exports[mid].name);
    if (!c) return rt_exports[mid].addr;
    if (c < 0) hi = mid;
    else lo = mid + 1;
  }
  return nullptr;
}
