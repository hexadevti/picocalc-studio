// export.h - turns a compiled program into a standalone firmware image for PicoMultiLoader.
//
// The firmware carries an empty export slot (image.h). An export copies the running image from
// flash to the SD card with the program's .pcx in the slot, the loader menu name set to the
// project and the checksum and SHA-256 redone. At boot, a firmware whose slot holds a program
// runs it instead of the IDE, through the same loader as F4 (rt_run).
#pragma once

#include <stddef.h>

// Writes the running firmware with `pcx` in its slot to `path` (written to a temporary file,
// checked like the bootloader checks it, then renamed). Returns the image size, or -1 with a
// message in err.
long ex_export(const void *pcx, size_t size, const char *name, const char *path, char *err, int errlen);

// True if this firmware is an exported program (its slot holds one).
bool ex_is_app();

// Runs the slot's program; when it ends, any key runs it again and Esc goes back to the loader.
// Does not return.
[[noreturn]] void ex_run_app();
