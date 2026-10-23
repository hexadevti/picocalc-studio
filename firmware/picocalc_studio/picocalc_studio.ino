// picocalc_studio.ino - PicoCalc Studio firmware for the ESP32-P4 PicoCalc.
// Phase 1: HAL + SDK. Runs a project built into the firmware (tools\build.ps1 -Project <name>)
// or, without one, the SDK demo. Build with tools\build.ps1, flash from PicoMultiLoader.
#include "board.h"
#include "src/hal/hal.h"
#include "src/sdk/picocalc.h"
#include "src/sdk/sdk_internal.h"
#include "src/rt/app_entry.h"
#include "src/rt/selftest.h"
#include "src/rt/module.h"
#include "src/ide/ide.h"
#include "src/export/export.h"

#ifdef ESP_PLATFORM
SET_LOOP_TASK_STACK_SIZE(32 * 1024);
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static char s_where[160], s_why[320];

// Shows the previous run's crash, if any. True if it was in a compiled program (s_where says where).
static bool showCrash() {
  char *where = s_where, *why = s_why;
  const bool inProgram = rt_module_crash_where(where, sizeof s_where);   // before the report clears it
  if (!hal_crash_report(why, sizeof s_why)) return false;
  hal_log("previous run: %s", why);
  if (inProgram) {
    hal_log("crashed in the program at %s", where);
    char b[320 + 200];
    snprintf(b, sizeof b, "Program crashed at %s. %s", where, why);
    snprintf(why, sizeof s_why, "%s", b);
  }
  gfx_clear(MAROON);
  gfx_text(4, 4, "The previous run crashed:", WHITE);
  int y = 20;
  for (const char *p = why; *p && y < SCREEN_H - 20; y += 10) {   // wrap at 52 columns
    char line[53];
    int n = 0;
    while (p[n] && n < 52) { line[n] = p[n]; n++; }
    line[n] = 0;
    gfx_text(4, y, line, YELLOW);
    p += n;
  }
  gfx_text(4, SCREEN_H - 12, "Any key: continue", LIGHTGRAY);
  gfx_flush();
  kbd_wait();
  return inProgram;
}

static void appMain() {
  hal_init();
  sdk_init();
  rt_selftest_install();
  const bool crashed = showCrash();
  hal_breadcrumb(rt_has_native_app() ? "native app" : ex_is_app() ? "exported app" : "ide");
  if (rt_has_native_app()) rt_run_native_app();
  if (ex_is_app()) ex_run_app();   // an exported program (F5): never the IDE
  ide_run(crashed ? s_where : nullptr, crashed ? s_why : nullptr);
}

#ifdef ESP_PLATFORM
void setup() { appMain(); }
void loop() {}
#else
// arduino-pico runs setup() on a 4 KB task: the app gets a task of its own.
static void appTask(void *) {
  appMain();
  vTaskDelete(nullptr);
}
void setup() {
  Serial1.setTX(0);
  Serial1.setRX(1);
  Serial1.begin(115200);
  if (xTaskCreatePinnedToCore(appTask, "app", 32 * 1024, nullptr, 1, nullptr, 0) != pdPASS)
    Serial1.println("boot: app task create FAILED");
}
void loop() { delay(1000); }
#endif
