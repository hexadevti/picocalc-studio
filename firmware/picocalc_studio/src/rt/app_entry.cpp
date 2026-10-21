// app_entry.cpp - runs a program built into the firmware (tools\build.ps1 -Project <name>, which
// copies the project's sources to src/app). Its C setup()/loop()/main() are weak here: in a
// build without a project they stay null and the firmware runs the built-in demo instead. The
// sketch's own setup()/loop() are C++ (mangled), so they don't collide.
#include "../sdk/picocalc.h"
#include "../sdk/sdk_internal.h"
#include "app_entry.h"

extern "C" {
__attribute__((weak)) void setup(void);
__attribute__((weak)) void loop(void);
// C++ may not name ::main; bind to the C symbol under another name.
__attribute__((weak)) int app_main_c(void) __asm__("main");
}

bool rt_has_native_app() { return setup || loop || app_main_c; }

void rt_run_native_app() {
  sdk_reset();
  if (app_main_c) {
    app_main_c();
    sdk_exit();
  }
  if (setup) setup();
  for (;;) {
    if (loop) loop();
    else sys_delay(100);
    sys_yield();
  }
}
