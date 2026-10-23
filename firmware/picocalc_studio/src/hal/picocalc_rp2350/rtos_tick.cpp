// rtos_tick.cpp - replaces the FreeRTOS tick interrupt of arduino-pico's RP2350 RISC-V port.
//
// The port's freertos_risc_v_mtimer_interrupt_handler (portASM.S, arduino-pico 6.1.1) calls
// xTaskIncrementTick() and then callTaskExitCriticalFromISR(), which returns void, before testing
// a0: the "a task woke up, switch to it" result is lost, so the tick never switches tasks. A task
// woken by a timeout (vTaskDelay, delay(), any blocking call with a timeout) then only runs when
// some other task on its core happens to yield; with everything blocked, the core idles forever.
// vPortSetupTimerInterrupt() is weak for exactly this: install a handler that keeps the result.
#if defined(ARDUINO_ARCH_RP2040) && defined(__riscv) && defined(__FREERTOS)
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "hardware/irq.h"
#include "hardware/riscv_platform_timer.h"
#include "hardware/structs/sio.h"

extern "C" {
// port.c's timer state, shared with its assembly.
extern uint64_t ullNextTime;
extern size_t uxTimerIncrementsForOneTick;
extern volatile uint64_t *pullMachineTimerCompareRegister;

// Called through the port's freertos_risc_v_interrupt_handler, which has saved the whole task
// context and restores whichever task pxCurrentTCB names afterwards.
static void __not_in_flash_func(tickIrq)() {
  volatile uint32_t *cmp = (volatile uint32_t *)pullMachineTimerCompareRegister;
  cmp[0] = 0xFFFFFFFFu;  // never below the old value while the high word changes
  cmp[1] = (uint32_t)(ullNextTime >> 32);
  cmp[0] = (uint32_t)ullNextTime;
  ullNextTime += uxTimerIncrementsForOneTick;

  const UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
  const BaseType_t switchNeeded = xTaskIncrementTick();
  taskEXIT_CRITICAL_FROM_ISR(saved);
  if (switchNeeded) vTaskSwitchContext((BaseType_t)get_core_num());
}

void vPortSetupTimerInterrupt(void) {
  riscv_timer_set_fullspeed(true);
  uxTimerIncrementsForOneTick = (size_t)(clock_get_hz(clk_sys) / configTICK_RATE_HZ);
  pullMachineTimerCompareRegister = (volatile uint64_t *)&sio_hw->mtimecmp;
  ullNextTime = riscv_timer_get_mtime() + uxTimerIncrementsForOneTick;
  *pullMachineTimerCompareRegister = ullNextTime;
  ullNextTime += uxTimerIncrementsForOneTick;
  irq_set_exclusive_handler(SIO_IRQ_MTIMECMP, tickIrq);
  irq_set_enabled(SIO_IRQ_MTIMECMP, true);
}
}
#endif
