// sdk_internal.h - glue between the SDK modules and the runtime. Not part of picocalc.h.
#pragma once

#include <stdint.h>

void sdk_init();                    // after hal_init(): framebuffer, sound task, RNG seed
void sdk_reset();                   // between programs: clip, font, key queues, sound off

void sdk_gfx_init();
void sdk_gfx_reset();
void sdk_kbd_reset();
void sdk_kbd_pump();                // move HAL key events into the SDK queues
void sdk_snd_init();
void sdk_snd_reset();

// What sys_exit() and the Break key do. The runtime installs its own; the default shows
// "program ended" and restarts on a key press.
typedef void (*sdk_exit_fn)(void);
sdk_exit_fn sdk_set_exit_handler(sdk_exit_fn fn);   // returns the previous one
[[noreturn]] void sdk_exit();
// Whether a Break press seen by the key pump calls sdk_exit() (the default). A debug run turns it
// off: Break pauses the program at its next stop point instead, and the pump drops the press.
void sdk_set_break_exits(bool on);
