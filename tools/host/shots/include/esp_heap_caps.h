// esp_heap_caps.h - every capability is the host heap.
#pragma once
#include <stdlib.h>

#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)

static inline void *heap_caps_malloc(size_t n, unsigned) { return malloc(n); }
static inline void *heap_caps_calloc(size_t k, size_t n, unsigned) { return calloc(k, n); }
static inline void *heap_caps_realloc(void *p, size_t n, unsigned) { return realloc(p, n); }
static inline void heap_caps_free(void *p) { free(p); }
