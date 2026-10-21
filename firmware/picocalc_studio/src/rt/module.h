// module.h - loads a linked .pcx image for execution on the device.
//
// The code goes into the free tail of the running ota_0 partition (past the end of the app image,
// so outside the image hash) and runs through an instruction mapping of that flash: heap memory
// isn't executable on the P4. Data and bss go into RAM. One module is loaded at a time.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "pcx.h"

struct RtModule {
  PcxView view;                // into the image the module was loaded from (keep it alive)
  uint8_t *text;               // executable address of the code
  uint8_t *ram;                // rodata + data + bss
  uint32_t mmap_handle;
  int (*main)(int, char **);
  void (*setup)(void);
  void (*loop)(void);
};

// Loads `image` (a cc_link() result or a .pcx file read into memory). `resolve` overrides
// rt_find_export() when it returns non-null (the runner's exit/abort). Returns 0, or -1 with a
// message in err. The previous module must have been unloaded.
int  rt_module_load(const void *image, size_t size, RtModule *m, void *(*resolve)(const char *name),
                    char *err, int errlen);
void rt_module_unload(RtModule *m);

// Source location of a code address inside the loaded module; false if it is outside.
bool rt_module_where(const RtModule *m, uint32_t pc, const char **file, int *line);

// After a panic reset: "file.c:line" of the crash if a module was loaded and the crash PC (or a
// return address on the stack) lies in its code. Once per crash; call before hal_crash_report().
bool rt_module_crash_where(char *buf, size_t n);
