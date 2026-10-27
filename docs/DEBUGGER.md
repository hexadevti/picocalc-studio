# On-device debugger: design

Status: step 1 of section 8 done on the host (branch `debug-feature`): `pcc -g` emits stop points
and the stops table, and simfw has the handler. Nothing runs on the device yet.

Goal: breakpoints, stepping, a call stack and variable watches for programs built and run from the
IDE, on both boards (ESP32-P4 and RP2350). Normal builds (F3/F4) and exports (F5) stay exactly as
they are today; everything below exists only in a **debug build**.

## 1. Mechanism: compiler-inserted stop points

No `ebreak` and no trap handler. In a debug build (`pcc -g`, `cc_debug`), `gen_stmt` emits a
*stop point* at the start of every statement, right after its `add_line()`:

```
    auipc t0, %hi(__dbg_flags + id)    ; PCX_R_PAIR relocation into the ram block
    lbu   t0, %lo(...)(t0)
    beqz  t0, 1f
    jal   t0, <stub>                   ; the object's stub, at the start of its text
1:
```

- `id` numbers the stop points of the image (0..n-1). `__dbg_flags` is a byte array of n entries
  that the linker puts at the end of bss: 1 = stop here. Each object numbers its stops from 0 and
  the linker adds the object's base when it resolves `__dbg_flags`.
- Always 16 bytes (4 instructions, 3 when the flag is 0). The beqz and jal are encoded directly,
  not through the branch relaxation, so the size never changes between the two passes.
- `jal t0` links through t0, so `ra` (still live in a leaf function that keeps it in a register)
  is untouched. t0 is free at a statement start: binary operators only use it inside one
  expression.
- No stop for `ND_BLOCK`, `ND_PACKED`, `ND_LABEL` and `ND_CASE` (their inner statement gets one)
  or inside a statement expression. The prologue's `add_line(fn->tok)` gets none, so `s0` is
  always valid when one runs. A line with several statements has several stops.
- Pass 0 of the two-pass emission takes `out->nstops` back in its rollback, as it does
  `out->nlines`.

The stub (emitted by codegen at the start of each debug object's text) saves **every** integer
register plus, on hard float, f0-f31 and fcsr into a `PcxDbgRegs` block (rt/pcx.h) on a 16-aligned
area of the program stack. `x[2]` is the sp of the stopped code and `x[5]` (t0) the address right
after the stop point's jal, so the firmware finds the stop id with `pcx_stop_index()` (a binary
search of the stops table) and the call needs no argument. It then calls the firmware import
`__pc_dbg_stop(PcxDbgRegs *)`, restores everything (registers changed in the block take effect)
and returns with `jalr zero, 0(t0)`.

Why not `ebreak`: it needs a resumable trap path in two different exception handlers (IDF panic on
the P4, `freertos_risc_v_application_exception_handler` on the RP2350), and code patching that
does not work for flash-mapped text. Stepping would also need temporary breakpoints planted at
every possible next line. The flag table makes stepping trivial (section 3) and works the same on
both boards. Debug builds are the only ones that pay for it.

## 2. Debug info in the .pcx image

The tables go in an optional trailer after the string table (4-byte aligned), so `PCX_VERSION`
stays 1 and a non-debug image is byte-for-byte what it was. The loader (`pcx_parse`) checks it and
sets `PcxView.dbg`/`.stops`/`.funcs`. Done so far: a `PcxDebug` header (`"PCXD"`, `n_stops`,
`flags_off` = the offset of `__dbg_flags` in the ram block, `n_funcs`), the stops as ascending text
offsets (their line comes from the existing line table), then the funcs as `PcxFunc` entries,
ascending, with their names in the string table. vars and types are still to do.

| table | entry | used for |
|---|---|---|
| stops | `{text_off, file, line}`, sorted by text_off | stop id <-> source line, breakpoint mapping |
| funcs | `{text_lo, text_hi, name, va, save_mask_x, save_mask_f}` | call stack, caller's register vars |
| vars  | `{name, type, loc_kind, loc, scope_lo, scope_hi}` | watches |
| types | `{kind, size, base/members, name}` | printing values |

- `loc_kind`: `FRAME` (`s0 + loc`), `REG` (x register number: s1..s11, or f register), `RAM`
  (offset into the ram block, for globals and statics), `BYREF` (frame slot holding a pointer).
- `scope_lo/hi`: the text range of the block the variable is visible in. Codegen notes `asm_pc()`
  when it enters and leaves an `ND_BLOCK` with locals. This handles shadowing: the innermost
  visible match wins.
- Types: scalars, pointers, arrays, structs/unions (members as `{name, type, offset}`), enums
  (values as names). Function pointers print as an address plus the function's name from funcs.
- The image is already held in RAM for the whole run (`rt_run(image, size, ...)`), so the tables
  are read in place and need no extra copy.

## 3. Runtime: stop, inspect, resume

`__pc_dbg_stop` runs on the **program task**. It stores the `PcxDbgRegs` pointer, notifies the
supervisor (`BIT_STOP`, next to `BIT_DONE`/`BIT_BREAK` in runner.cpp) and blocks on a binary
semaphore (not a task notification, which a driver could also use). The supervisor (the IDE's task) runs the debugger UI while the program sits blocked
in a known state. It can read the program's memory and saved registers safely, and the program
stack is not used for UI code.

Deciding whether to actually stop, in `__pc_dbg_stop`, before notifying anyone (rt/pcx_dbg.c,
shared with the simulator's pcxrun so the host tests cover it):

```
any mode       : stop if bp[id]                          (RUN: flags = breakpoints only)
mode STEP_INTO : stop if line(id) or file differs, or s0 != start s0
mode STEP_OVER : s0 == start s0: stop if line or file differs; else stop if s0 > start s0
mode STEP_OUT  : stop if s0 > start_s0
mode PAUSE     : stop at once
```

A breakpoint is set on the **first** stop of its line only: `int b = f(a);` has two (the
declaration and its initializer), and stepping already treats a line as one place.

Every callee of one function shares one `s0` (the caller's sp), so a stop point alone can't
tell that a stepped function returned: with no stop left in the caller after the call
(`return f(n-1) + f(n-2);`), `finish` would stop in the next call at that depth. So **any step
also catches the return** of the frame it starts in: `pcx_dbg_resume` saves the frame's real ra
(`s0 - va - 4`, `va` from the funcs entry) in a 16-entry list and patches the slot to
`pcx_dbg_ret_tramp` (rt/pcx_dbg_ret.S). The function returns there with the caller's registers;
the trampoline saves them like the stop stub and calls `__pc_dbg_return`, which puts the real ra in
`x[5]` and decides (`pcx_dbg_return`): stop (shown as "return", at the return site, on the call's
line) if the returning frame is the stepped one (`next`/`step`/`finish`) or a pause is pending.
The trampoline then jumps to `x[5]`. The list entry is keyed by `s0`; a return also drops entries
below it (frames a longjmp skipped), and the backtrace reads through patched slots. Not caught:
main's return (its ra is in the firmware), so `finish` in main runs to the end.

A place in the program is a text offset inside its line: a stop point's offset, or a return
address minus 1. The stopped view, the backtrace and `pcx_dbg_resume` all take places.

For any mode other than RUN, all flags are set to 1 (memset), and the stop handler filters. The
stack grows down, so a caller frame has a higher `s0`, and recursion works without extra state.
Returning to RUN restores the flags from the breakpoint set.

**Break key in a debug run = pause**: set every flag, and the program stops at its next statement,
even in a tight loop with no SDK call. `sdk_set_break_exits(false)` makes the SDK's key pump drop
the press instead of ending the program. A second Break before the program reaches a stop (it is
in a firmware call, or in code built without stops) ends it the usual way: Break exits again,
the breakpoints go, and the 500 ms kill stays as the last resort.

Call stack (`pcx_dbg_backtrace`, done): start from the stopped place and `s0`. A frame's saved ra
is at `s0 - va - 4` and the saved s0 is at `s0 - va - 8`, with `va` taken from the funcs entry.
Stop at an address outside the image text (main's caller is the firmware). Still to do, with the
locals: a caller's register variables come from the callee's save area (`s0 - va - 12 - 4k`, in
save-mask order), exactly as the epilogue reloads them.

## 4. Screen and keys while stopped

- Programs draw only into the framebuffer and push it with `gfx_flush*`. The debugger draws
  straight to the LCD with `hal_lcd_push`/`hal_lcd_fill`, so the framebuffer is untouched. On
  resume, `hal_lcd_push` of the whole framebuffer restores the program's picture.
- Keys: the program is not polling while it is stopped, so the debugger reads `hal_key_poll`
  itself. On resume it drains the queue, so a debugger key never reaches the program.
- Source text: on the RP2350 the docs are parked (freed) during a run. The debugger reads the few
  lines around the stop from the saved file on the SD card. Debug runs require saved files, as
  F3/F4 already do.

Stopped view (rt/debugger.cpp, 6x10 font = 53x32 cells): a title bar (file:line and why it
stopped: breakpoint, step, return, paused; or the selected frame's number), 20 source lines with
the current line a third of the way down and highlighted, colored like the editor (doc.cpp's
`hl_scan`), `*` marks on breakpoint lines, the call
stack (a title bar and 9 frame rows, `#N function file:line`, innermost first, scrolling to the
selected frame; up to 32 frames), and a key line. PgUp/PgDn select a frame, and the source shows
its line. It draws a few cells at a time from a stack buffer and reads the source line by line, so
it needs no heap. Later: the watches and the locals of the selected frame. Keys (it also takes
c/n/s/f/q):

| key | action |
|---|---|
| F4 / Enter | continue |
| Down | step over |
| Right | step into |
| Left | step out |
| PgUp / PgDn | select a frame (inward / outward) |
| Tab | locals / watches pane; Enter expands a struct or array |
| W | add a watch (a name; expressions come later) |
| Esc / Break | stop the program |

In the editor: **Ctrl-B** toggles a breakpoint on the cursor line (the line number turns red)
and **Ctrl-F4** builds in debug mode and runs (Shift-F4 is F9 on this keyboard). Breakpoints are
kept per Doc as line numbers (up to 16 per file) and move with line inserts and deletes
(doc.cpp's rawInsert/rawDelete, so undo moves them too). At run start each maps to the first stop
of its line; a breakpoint on a line with no stop moves to the next line that has one.

Not handled yet: sound keeps playing while the program is stopped.

## 5. Live variable monitoring

- **When stopped:** everything, both locals and globals.
- **While running:** globals only. The supervisor wakes every 200 ms and reads the watched globals
  (it outranks the program on the same core, so the reads are consistent, apart from a value the
  program is halfway through writing). **Decided: an overlay strip** of 2-4 lines pushed to the
  LCD after the program's own flush. It flickers when the program redraws that area.

## 6. Memory

- ESP32-P4: PSRAM, no concern.
- RP2350: a debug image is bigger (see the measurement below), plus about 3 bytes of tables per stop and about
  16 bytes per variable. Small and medium projects fit. Asteroids builds today with about 1 KB of
  headroom, so the biggest projects may only fit with debug info for chosen files (a per-file
  option: the files with no debug info just have no stops). **Decided: accepted.**
- Measured on the host (step 1): the test and bench images grow about 1.9x with `-g` (stops only).
  With every flag 0, the benchmarks run 1.1-1.85x the instructions of a normal build (tight loops
  pay the most). The compiler side can use the lent
  framebuffer as it already does.

## 7. Testing on the host (before any flash)

`rvsim` + simfw already run pcc output. Add `__pc_dbg_stop` to simfw:

1. **Transparency (done):** every unit test with `-g`, all flags 0, then all flags 1
   (`pcxrun --dbg all`, a handler that checks the stop address and sp, then returns), on hard and
   soft float. The output must match the `.expected` file:

   ```
   run_tests.py pcc --pcc "build/host/pcc.exe -g {src} -o {out}" --exec rvsim --pcxrun-args "--dbg all"
   ```

   `--dbg trace` also prints `stop file:line` on stderr for each stop.
2. **Script tests (done for stepping and the call stack):** `pcxrun --dbg SCRIPT` runs
   tests/debug/NAME.dbg, with `break`/`delete FILE:LINE`, `run`/`continue`, `next`, `step`,
   `finish`, `bt` (`print x` comes with step 4). It prints `@break`/`@stop`/`@return FILE:LINE`
   lines (and `@bt FUNC FILE:LINE` per frame) between the program's output, checked against
   NAME.expected. d03_ret checks that values come back through the trampoline (int, float,
   long long, a struct, a variadic function):

   ```
   run_tests.py debug --pcc "build/host/pcc.exe -g {src} -o {out}"
   ```
3. Memory: `mem.py` with `--debug` for the build peak, and image sizes before and after.

## 8. Order of work

1. ~~Codegen stop points + stops table + `-g` in pcc; simfw handler; transparency test.~~ Done.
2. ~~runner.cpp stop/resume protocol, PAUSE on Break, minimal stopped view (source + current line,
   continue/step/stop). IDE: Ctrl-B, Ctrl-F4.~~ Done, tested on hardware.
3. ~~funcs table + call stack, return trampoline for finish.~~ Written, host tests pass, both
   firmwares build. *Hardware test: next.*
4. vars/types tables + locals, globals and the watch list.
5. Live globals while running (the overlay strip).
6. Later: watch expressions and conditional breakpoints (reuse the parser's expression code and
   evaluate against the saved registers), data breakpoints (Hazard3 triggers, maybe).
