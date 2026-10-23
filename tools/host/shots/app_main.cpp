// app_main.cpp - a setup()/loop() program built natively with the SDK, for screenshots (see
// shots.py). These are gcc builds: they show what a program draws, not that pcc compiles it.
#include "../../../firmware/picocalc_studio/src/hal/hal.h"
#include "../../../firmware/picocalc_studio/src/sdk/sdk_internal.h"

extern "C" void setup(void);
extern "C" void loop(void);

static void quit() { exit(0); }

int main() {
  hal_init();
  sdk_init();
  sdk_set_exit_handler(quit);
  setup();
  for (;;) loop();
}
