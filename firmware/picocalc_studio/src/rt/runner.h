// runner.h - builds and runs compiled programs on the device.
//
// rt_run() loads a .pcx image (rt/module.h) and runs it in its own task while the calling task
// supervises it. The program ends when main() returns, on exit()/abort()/sys_exit(), on Break, or
// at the timeout. Break first asks nicely (the program's next SDK call exits); a program that
// makes no SDK call for 500 ms is deleted outright. Memory, files and directories the program
// opened through malloc/fopen/opendir are released afterwards. One program at a time.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct RtRunOptions {
  int argc;
  char **argv;
  FILE *out;            // the program's stdout, or null for the console
  bool foreground;      // owns the screen, keys and sound: sdk_reset() before and after
  uint32_t timeout_ms;  // 0 = none
};

enum RtEnd { RT_END_RETURN, RT_END_EXIT, RT_END_ABORT, RT_END_BREAK, RT_END_KILLED, RT_END_TIMEOUT, RT_END_BAD_FREE };

struct RtRunResult {
  int status;               // main()'s return value or exit()'s argument; 134 for abort
  RtEnd end;
  uint32_t ms;
  int freed_blocks;         // left allocated by the program, freed by the runtime
  size_t freed_bytes;
  int closed_files;         // left open by the program (files + directories)
  char err[160];            // load error (rt_run returned -1) or what went wrong
};

// Returns 0 if the program ran (see result), -1 if it could not be loaded (result->err).
int rt_run(const void *image, size_t size, const RtRunOptions *opt, RtRunResult *result);

const char *rt_end_name(RtEnd end);

// Compiles and links source files into a .pcx image (malloc'ed, caller frees). Headers come from
// /sd/pico-dev/include. Returns null with the message in cc_error().
uint8_t *rt_build(const char *const *paths, int n, size_t *size);

// rt_build() on a temporary 64 KB task: the compiler recurses deeply. Blocks the caller.
uint8_t *rt_build_big(const char *const *paths, int n, size_t *size);

// The .c files of a project directory, sorted, as strdup'ed full paths. Returns the count.
int rt_list_sources(const char *dir, char **files, int max);

// "@run <file.c | project dir> [times]": queued from the serial port, run in the foreground by
// whoever owns the screen when it calls rt_service() (the IDE or the demo menu). Returns true if it
// ran something (the caller redraws).
bool rt_run_cmd(const char *line);
bool rt_service();
