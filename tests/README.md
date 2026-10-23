# PicoCalc Studio compiler test suite

Acceptance gate for the on-device C compiler (`firmware/picocalc_studio/src/cc`).
Copied to `/sd/picocalc-studio/tests/` and `/sd/picocalc-studio/projects/` on the SD card.

| Dir | What | How it's checked |
|---|---|---|
| `unit/` | Tier A: `t01`–`t21`, one `.c` + `.expected` each, print a transcript and return 0 | stdout must equal `.expected` |
| `errors/` | `err01`–`err12`, must fail; first line `// EXPECT: <compile\|link> line=<n\|m> match=<regex>` | diagnostic kind, line and message |
| `projects/` | Tier B: full SDK apps (`setup`/`loop`), each with `project.cfg` | build + run by hand / `@shot` on device |

## Host harness: `tools/host/run_tests.py`

```
python tools/host/run_tests.py ref [--update]   # host gcc (i686, ILP32) runs units, diffs .expected
python tools/host/run_tests.py target           # riscv32-esp-elf-gcc -Wall -Werror: units + projects
python tools/host/run_tests.py errors           # gcc must reject each err file at the stated line
python tools/host/run_tests.py pcc --pcc "cmd {src} {out}" --exec "cmd {out}"   # our compiler
python tools/host/run_tests.py all
```

`ref` and `target` validate the tests themselves (outputs are portable, sources are valid for
RV32 ilp32f and the struct-layout `_Static_assert`s hold); `pcc` runs the same files through
the PicoCalc compiler once it exists.

## Tier B projects

| Project | Exercises |
|---|---|
| hello | setup/loop, text, `kbd_get`, `sys_exit`; first export target |
| multi_file | 3 units + header, cross-file globals, same-named `static` functions |
| keytest | `kbd_event` raw press/held/release, `kbd_down`, modifiers |
| paint | every gfx primitive, `gfx_flush_rect`, direct framebuffer XOR |
| snake | fixed timestep, ring buffer of structs, random, SD high score |
| tetris | const 2-D/3-D tables, bit tricks, `memmove` line clears, sound |
| mandelbrot | float (FPU) vs double (soft-float) benchmark + checksum |
| synth | `snd_play` streaming, 64-bit math, envelope, waveforms |
| filebrowser | `opendir`/`readdir`/`stat`, `qsort` callback, `realloc`, file viewer |
| textedit_mini | heap-allocated lines, `realloc`/`memmove`, load/save |
| battery_mon | `sys_battery`, LCD/keyboard backlight, periodic redraw |
| crash_test | NULL write, endless loops (Break), stack overflow, div-by-0, illegal insn, `abort`, bad `free` |
| stress_alloc | leaks heap/files/sound on purpose; run 50x, heap must return to baseline |
