// ide_main.cpp - the IDE as a PC program, for screenshots (see shots.py).
#include "../../../firmware/picocalc_studio/src/hal/hal.h"
#include "../../../firmware/picocalc_studio/src/sdk/sdk_internal.h"
#include "../../../firmware/picocalc_studio/src/ide/ide.h"

int main() {
  hal_init();
  sdk_init();
  ide_run(nullptr, nullptr);
}
