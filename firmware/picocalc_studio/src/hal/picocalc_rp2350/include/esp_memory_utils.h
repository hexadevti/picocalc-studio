// esp_memory_utils.h - ESP-IDF address classification (RP2350 build only): SRAM is all there is.
#pragma once
#include <stdbool.h>
#include <stdint.h>

static inline bool esp_ptr_internal(const void *p) {
  return (uintptr_t)p >= 0x20000000u && (uintptr_t)p < 0x20082000u;
}
static inline bool esp_ptr_external_ram(const void *p) { (void)p; return false; }
