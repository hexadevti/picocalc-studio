// task.h - sys.cpp's yields, as no-ops (single-threaded harness).
#pragma once
static inline void vTaskDelay(int) {}
static inline void taskYIELD() {}
