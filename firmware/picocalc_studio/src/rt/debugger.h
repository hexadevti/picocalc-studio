// debugger.h - the stopped view of a debug run (docs/DEBUGGER.md).
#pragma once

#include "module.h"
#include "pcx.h"

// Draws where the program stopped (the place `at`, pcx.h; why: "breakpoint", "step", "return",
// "paused") and its call stack over the screen, and waits for a key. Returns the PCX_DBG_* mode to
// resume in, or -1 to end the program. The program's picture (its framebuffer) is back on the
// screen when it returns. Runs on the supervisor while the program task waits.
int rt_dbg_view(const RtModule *m, const PcxDbg *d, uint32_t at, const PcxDbgRegs *regs,
                const char *why);
