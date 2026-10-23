// esp_heap_caps.h - the few ESP-IDF heap_caps calls the shared code makes, on a plain malloc heap
// (RP2350 build only: one SRAM, no PSRAM, so every capability is the same heap).
#pragma once
#include <stdlib.h>
#include <malloc.h>

#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_DMA      (1u << 3)
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)

#ifdef __cplusplus
extern "C" {
#endif
size_t rp_free_heap(void);   // system.cpp
#ifdef __cplusplus
}
#endif

static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t k, size_t n, unsigned caps) { (void)caps; return calloc(k, n); }
static inline void *heap_caps_realloc(void *p, size_t n, unsigned caps) { (void)caps; return realloc(p, n); }
static inline void *heap_caps_aligned_alloc(size_t align, size_t n, unsigned caps) { (void)caps; return memalign(align, n); }
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return rp_free_heap(); }
