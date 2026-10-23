// FreeRTOS.h - ESP-IDF's "freertos/FreeRTOS.h" on arduino-pico's FreeRTOS SMP (RP2350 build only;
// tools\build_rp2350.ps1 puts this include dir first). Adds the ESP-IDF extras the shared code uses.
#pragma once
#include <FreeRTOS.h>
#include <task.h>
#include "hardware/sync.h"

// ESP-IDF counts task stacks in bytes, this FreeRTOS in words.
#define xTaskCreatePinnedToCore(fn, name, stackBytes, arg, prio, handle, core) \
  xTaskCreateAffinitySet((fn), (name), (configSTACK_DEPTH_TYPE)((stackBytes) / sizeof(StackType_t)), (arg), (prio), \
                         (UBaseType_t)(1u << (core)), (handle))

static inline BaseType_t xPortGetCoreID(void) { return (BaseType_t)get_core_num(); }
