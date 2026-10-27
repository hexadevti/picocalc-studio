# PicoCalc Studio

A C development environment that runs **on the [ClockworkPi PicoCalc](https://www.clockworkpi.com/picocalc)**.
You write, build, run and package programs on the device itself, with no PC involved: an editor
with completion and go-to-declaration, a C compiler that targets the board's RISC-V CPU, a runner
that catches crashes, and an exporter that turns a program into a standalone firmware for
[PicoMultiLoader](https://github.com/hexadevti/PicoMultiLoader), the SD-card boot menu we recommend for
the PicoCalc (see [Installing](#installing)).

<p align="center">
  <img src="docs/screenshots/ide-editor.png" width="320" alt="The editor">
  <img src="docs/screenshots/app-kart.png" width="320" alt="The kart example running">
</p>

- **Editor**: syntax colours, Ctrl-Space completion from the SDK headers and your project, F9 go to declaration (into the headers and the SDK sources too), find/replace, undo/redo, selection and clipboard, multi-file projects.
- **Compiler**: a fork of [chibicc](https://github.com/rui314/chibicc) with an RV32 code generator and linker of its own (C11, `float` on the FPU, `double` in software). Builds take from a fraction of a second to a few seconds. Errors land on the line, in the status bar.
- **Runner**: F4 builds and runs. Break stops the program. Files, memory and sound it leaves behind are cleaned up. If it crashes, the IDE reopens at the line where it crashed.
- **Export**: F5 writes `<project>.bin` (ESP32-P4) or `<project>.uf2` (Pico 2 W), a copy of the firmware that boots straight into your program. PicoMultiLoader flashes it like any other app.
- **SDK**: `picocalc.h`, which covers a 320×320 framebuffer with drawing primitives and fonts, the keyboard, sound (tones and streamed samples), time, battery and backlights. You also get a C library (stdio, files and directories on the SD card, `malloc`, `math.h`, …).
- **19 examples**, from `hello` to a mode-7 kart racer, a voxel flight game and a polygon space shooter.

## Boards

The same firmware source builds for two modules in the PicoCalc's Pico socket. The core you build
with picks the board (`board.h`).

| | **ESP32-P4** (Waveshare ESP32-P4-WIFI6) | **Raspberry Pi Pico 2 W** (RP2350) |
|---|---|---|
| Role | main target, every feature tested on hardware | port. Runs on hardware but is limited by RAM. |
| CPU | ESP32-P4: 2× RISC-V at 360 MHz (chip v1.x) or 400 MHz (v3.x), single-precision FPU | Hazard3 RISC-V at 150 MHz, no FPU |
| `float` | hardware FPU | software (soft-float) |
| `double` | software | software |
| Memory | 32 MB PSRAM in the package: the framebuffer, the compiler and programs all fit with room to spare | 520 KB SRAM, no PSRAM. About 400 KB of heap after the firmware. |
| Compiler memory | its own heap in PSRAM | borrows the 200 KB framebuffer during a build, and parks the open files to free their memory |
| Program memory | MBs (PSRAM) | about 140 KB for code + data + a 32 KB stack + `malloc` |
| Where compiled code runs | from flash (mapped tail of the app partition) | from SRAM (relocated in place) |
| LCD SPI clock | 80 MHz | 37.5 MHz (F_CPU / 4): full-screen flushes take about twice as long |
| Audio | I2S PDM into the mainboard's filter | PWM on GPIO26/27 |
| Flash | 32 MB; the app runs from PicoMultiLoader's 10 MB app slot | 4 MB; the app area is 4088 KB after the loader's 8 KB boot stub |
| F5 export | `.bin`, tested on hardware | `.uf2`, written but not yet tested on hardware |
| Crash → file:line | yes, also after a reboot | live only. After a reboot it is printed on the serial port. |
| Compiler self-test (`@test`) | all pass | all pass (the biggest test compiles in 4 s) |
| Examples | all 19 | the small and medium ones. asteroids builds (3 s) and demoscene runs. **kart** and **starfighter** need more memory than is free. |
| Build | `tools\build.ps1` (arduino-esp32) | `tools\build_rp2350.ps1` (arduino-pico 6.1.1, RISC-V, FreeRTOS) |
| Loader | [PicoMultiLoader](https://github.com/hexadevti/PicoMultiLoader), as it is | [PicoMultiLoader](https://github.com/hexadevti/PicoMultiLoader) with the boot stub that can start a RISC-V image |

On the Pico 2 W, memory decides what fits, not speed: a build needs the compiler's peak (up to
about 210 KB for the larger examples), the open documents and the IDE all at once. `python
tools/host/mem.py` measures a project's compiler peak on the PC.

## Screenshots

### The IDE

| | |
|---|---|
| ![Project list](docs/screenshots/ide-projects.png) | ![Editor](docs/screenshots/ide-editor.png) |
| **Projects**: your own (My files) and the read-only examples. "+ New from example" copies one to change. | **Editor**: line numbers, syntax colours, the file and battery in the title bar. |
| ![Completion](docs/screenshots/ide-complete.png) | ![Go to declaration](docs/screenshots/ide-decl.png) |
| **Ctrl-Space** completes from C, the headers and the project. The status bar shows the chosen name's declaration. | **F9** on `gfx_fill_rect` opens the SDK's own source, read-only. Esc comes back. |
| ![Build OK](docs/screenshots/ide-build.png) | ![Build error](docs/screenshots/ide-error.png) |
| **F3** builds. The output panel shows what happened. | A build error marks the line and points at the column. |
| ![Files](docs/screenshots/ide-files.png) | ![Help](docs/screenshots/ide-keys.png) |
| **Esc**: the project's files, and the headers and SDK sources it uses. | **F1**: the keys, and every header the compiler offers. |

### Example programs

| | |
|---|---|
| ![kart](docs/screenshots/app-kart.png) | ![voxel](docs/screenshots/app-voxel.png) |
| **kart**: mode-7 racing against five computer drivers | **voxel**: flying over height-field terrain, Comanche style |
| ![starfighter](docs/screenshots/app-starfighter.png) | ![raycaster](docs/screenshots/app-raycaster.png) |
| **starfighter**: a flat-shaded polygon shooter | **raycaster**: a textured maze, Wolfenstein 3D style |
| ![demoscene](docs/screenshots/app-demoscene.png) | ![grapher](docs/screenshots/app-grapher.png) |
| **demoscene**: plasma, copper bars and a sine scroller, synced to a chiptune made on the fly | **grapher**: plots up to four functions with symbolic derivatives |
| ![solids](docs/screenshots/app-solids.png) | ![mandelbrot](docs/screenshots/app-mandelbrot.png) |
| **solids**: rotating 3D meshes with lighting | **mandelbrot**: `float` vs `double` speed |
| ![asteroids](docs/screenshots/app-asteroids.png) | ![tetris](docs/screenshots/app-tetris.png) |
| **asteroids**: vector graphics with phosphor trails | **tetris** |

> These screenshots come from [`tools/host/shots`](#screenshots-1), which builds the real IDE and SDK for
> the PC. The pixels match what the device draws. Timings that programs print (build ms, "3D ms")
> are the PC's; frame rates are close to the device's, because the harness's LCD takes as long as
> the real one.

## Installing

### Use PicoMultiLoader to host the PicoCalc

We recommend **[PicoMultiLoader](https://github.com/hexadevti/PicoMultiLoader)** as the PicoCalc's
firmware host. It is an SD-card boot menu for the PicoCalc: you copy apps to the microSD card,
pick one on the LCD with the keyboard, and the loader flashes and boots it. It runs on all the
boards PicoCalc Studio supports: the ESP32-P4 (`.bin` apps), the Pico 2 W (`.uf2` apps) and the
RP2040 Pico.

PicoCalc Studio is built to live under it:

- **It is a PicoMultiLoader app.** The firmware is a plain app image that the loader writes to its
  app slot. It never touches the bootloader or the partition table.
- **F10 (or Shift-F5 on any screen) goes back to the loader**, through the loader's own "show the
  menu" signal. You can also hold **Up** (or F1/F5) while switching on or resetting.
- **F5 exports are PicoMultiLoader apps too.** An exported program is a copy of the firmware that
  boots straight into that program. You put it in the loader menu like any other app, without a PC.
- **The loader and Studio share the SD card.** The loader's web file manager (F4, on the Wi-Fi
  boards) and USB drive mode (F5) can copy your projects to and from the PC.

Don't flash PicoCalc Studio with `arduino-cli upload` or esptool. On the P4 that also writes the
bootloader and partition table, which removes the loader.

### 1. Install PicoMultiLoader (once)

Follow the [PicoMultiLoader README](https://github.com/hexadevti/PicoMultiLoader#readme):

- **ESP32-P4**: flash the loader over USB with `idf.py flash`. Build it for your chip's revision
  (v3.x by default).
- **Pico 2 W**: hold BOOTSEL, copy `PicoMultiLoader-rp2350.uf2` to the `RP2350` drive, then
  copy `pml2350.uf2` to the root of the SD card. PicoCalc Studio is a **RISC-V** build, so the
  boot stub must be able to start RISC-V images. Use a loader build that includes the RISC-V
  boot stub.

### 2. Put PicoCalc Studio on the SD card

1. Build the firmware (see [Building](#building)), or take a release build:
   `picocalc_studio.bin` (ESP32-P4) or `picocalc_studio.uf2` (Pico 2 W).
2. Copy this repo's [`sdcard/picocalc-studio`](sdcard/picocalc-studio) folder to the root of the
   SD card. It has the C headers, the SDK sources (for F9), the examples and the compiler tests.
3. Copy the firmware file into that `picocalc-studio/` folder too, keeping its name
   (`picocalc_studio.uf2` / `picocalc_studio.bin`).
4. In PicoMultiLoader, pick **Studio** on the Home screen and press **Enter** twice (details,
   then flash and run). The next time you switch on, Studio starts directly.

The folder is a ready-made PicoMultiLoader app (see its
[docs/apps.md](https://github.com/hexadevti/PicoMultiLoader/blob/master/docs/apps.md)):
`app.ini` gives Home the name, version, category and description and picks the right binary
for the board, and `icon.bmp` is the 32×32 icon, drawn in palette roles so it follows the
loader's theme. The P4 build also writes the name and version (`firmware/picocalc_studio/version.h`)
into the app descriptor, which the loader's details screen shows. When you change the version,
change it in `app.ini` too.

Any other loader works the same way: it ignores `app.ini` and `icon.bmp`, so browse to
`picocalc-studio/` and pick the firmware file there. On the Pico 2 W, "back to the menu" (F10,
Shift-F5) uses the watchdog request that [uf2loader](https://github.com/pelrun/uf2loader) defined
and PicoMultiLoader also uses. Without a loader, it just restarts.

Apps you export with F5 go to `/picocalc-studio/apps/` by default. PicoMultiLoader's Home only
lists folders in the root of the card, so either browse to them in Files (F2 adds one to Home as
a favourite) or export into a top-level folder of their own, such as `/MyGame`.

SD card layout:

```
/picocalc-studio/
  app.ini       PicoMultiLoader metadata: name, version, which binary
  icon.bmp      PicoMultiLoader Home icon
  picocalc_studio.uf2 / .bin   the firmware (you copy it here)
  include/      C and SDK headers (picocalc.h)
  lib/picocalc/ SDK sources, shown read-only by F9
  examples/     the examples (read-only in the IDE)
  projects/     your projects: one folder each, every .c file in it is built
  apps/         default folder for F5 exports
  tests/        compiler self-test (@test)
  ide.cfg       the last project and file
```

## Writing a program

A program is a folder in `projects/` with one or more `.c` files. It has `setup()` and `loop()`
(or a plain `main()`):

```c
#include <picocalc.h>
#include <stdio.h>

void setup(void)
{
    gfx_clear(NAVY);
    gfx_font(FONT_7X13);
    gfx_text(20, 20, "Hello, PicoCalc!", WHITE);
    gfx_flush();
}

void loop(void)
{
    int k = kbd_get();
    if (k == KEY_ESC) sys_exit();
    if (k != KEY_NONE) printf("key %d\n", k);   /* printf shows in the output panel (F7) */
    sys_delay(16);
}
```

The SDK at a glance ([`picocalc.h`](sdcard/picocalc-studio/include/picocalc.h) has the details,
and F1 shows it on the device):

| | |
|---|---|
| **gfx_** | `clear` `pixel` `get_pixel` `line` `hline` `vline` `rect` `fill_rect` `round_rect` `fill_round_rect` `circle` `fill_circle` `triangle` `fill_triangle` `blit` `scroll` `clip` `noclip` `font` `text` `text_bg` `printf` `text_width` `flush` `flush_rect`, and `gfx_framebuffer()` for direct pixel access (RGB565, 320×320) |
| **kbd_** | `get` `wait` `event` (raw press/release) `down` (is it held) `mods` `flush` `backlight` |
| **snd_** | `tone` `play` (stream 16-bit samples) `busy` `stop` `volume` |
| **sys_** | `millis` `micros` `delay` `yield` `random` `seed` `battery` `backlight` `free_heap` `log` `exit` `exit_to_loader` |

To use code from another project, `#include "../other/x.h"`. Its `x.c` is then built with your
project.

## IDE keys

| Key | | Key | |
|---|---|---|---|
| F1 | help, headers | Ctrl-Space | completion |
| F2 | mark mode (select) | Ctrl-S | save all |
| F3 | build | Ctrl-G | go to line |
| F4 | build and run (Break stops) | Ctrl-F / Ctrl-N | find / next |
| F5 | export `.bin` / `.uf2` | Ctrl-R | replace |
| F6 | full output | Ctrl-Z / Ctrl-Y | undo / redo |
| F7 | output panel on/off | Ctrl-K / Ctrl-D | delete / duplicate line |
| F8 | back to the last edit | Ctrl-C / X / V | copy / cut / paste |
| F9 | go to declaration | Alt + arrows | select |
| F10 | back to PicoMultiLoader | Tab / Alt-Tab | indent / unindent |
| Esc | the project's files | Ctrl-A | select all |

## Building

Needs [arduino-cli](https://arduino.github.io/arduino-cli/) with the cores below. Neither script
uploads anything.

```powershell
tools\build.ps1           # ESP32-P4 (arduino-esp32)       -> build\picocalc_studio_p4\picocalc_studio.bin
tools\build_rp2350.ps1    # Pico 2 W (arduino-pico, RISC-V) -> build\picocalc_studio_rp2350\picocalc_studio.uf2
```

`tools\build.ps1 -Project <name>` compiles a `tests/projects` program into the firmware with gcc.
That gives a reference build to compare against the on-device compiler.

## Testing on the PC

The compiler is plain C. It builds and runs on the PC, and its RV32 output runs in a small emulator:

```sh
python tools/host/build_host.py      # build/host: pcc.exe (the compiler), rvsim.exe (RV32 emulator), pcxrun.elf
python tools/host/run_tests.py       # validate the tests themselves (host gcc, riscv32 gcc, rvsim)
python tools/host/run_tests.py pcc --pcc "<abs path>/build/host/pcc.exe {src} -o {out}" --exec rvsim
python tools/host/bench.py           # code speed (instruction counts) against tests/bench/baseline.json
python tools/host/mem.py             # compiler memory peak per project (matters on the Pico 2 W)
```

On the device, `@test` sent over the USB serial port runs the same test suite. See
[tests/README.md](tests/README.md).

### Screenshots

```sh
python tools/host/shots/shots.py              # every screenshot in docs/screenshots/
python tools/host/shots/shots.py ide-error    # just one
```

`tools/host/shots` builds the IDE (with the real compiler) and each example natively with MinGW
gcc. The PC stand-in for the HAL draws into memory and replays a key script. Each shot is a short
script in `shots.py` (`key`, `ctrl`, `type`, `hold`, `wait`, `shot`). Running a program inside the
IDE isn't supported there, so each example is built with gcc as its own program instead. It needs
gcc/g++ and Pillow.

## Repository layout

```
firmware/picocalc_studio/     the firmware (Arduino sketch)
  src/cc/                     the C compiler: chibicc fork, RV32 codegen, assembler, linker
  src/ide/                    editor, documents, completion, the IDE
  src/ui/                     menus, lists, text views
  src/rt/                     module loader, runner, exports table, self-test
  src/sdk/                    picocalc.h implementation (gfx, kbd, snd, sys), fonts
  src/export/                 F5: firmware image with the program in its slot
  src/hal/picocalc_p4/        ESP32-P4: display, keyboard, audio, system
  src/hal/picocalc_rp2350/    Pico 2 W: the same, plus POSIX on SD, module loader, export
firmware/spikes/              feasibility spikes (docs/PHASE0.md)
sdcard/picocalc-studio/       what goes on the SD card
tests/                        compiler tests: unit, errors, projects, benchmarks
tools/                        build scripts, export table generator
tools/host/                   PC tools: pcc, rvsim, test runner, benchmarks, screenshots
docs/                         design notes, screenshots
```

## Third-party code

The compiler is based on chibicc (MIT). The fonts are public domain, and the firmware is built
on the Arduino cores for ESP32 and RP2350. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
