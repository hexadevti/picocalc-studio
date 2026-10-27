# Third-party notices

PicoCalc Studio includes or is derived from the third-party works below. Each keeps its own
license.

## Included in the source

### chibicc (MIT)

`firmware/picocalc_studio/src/cc/` is a fork of [chibicc](https://github.com/rui314/chibicc) by
Rui Ueyama, at commit `90d1f7f199cc55b13c7fdb5839d1409806633fdb`
(see [`CHIBICC_COMMIT`](firmware/picocalc_studio/src/cc/CHIBICC_COMMIT)). The tokenizer,
preprocessor, parser and type checker come from chibicc and were changed for the device:
streaming preprocessing, per-statement compilation, arena memory and soft-float. The RV32 code
generator, assembler and linker are new. The license is in
[`src/cc/LICENSE`](firmware/picocalc_studio/src/cc/LICENSE):

```
MIT License

Copyright (c) 2019 Rui Ueyama

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### wcwidth.c (permissive)

The character width table in `src/cc/unicode.c`, which came with chibicc, is based on
[Markus Kuhn's `wcwidth.c`](https://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c). Its terms:
"Permission to use, copy, modify, and distribute this software for any purpose and without fee
is hereby granted. The author disclaims all warranties with regard to this software."

### font8x8 (public domain)

`firmware/picocalc_studio/src/sdk/font8x8.h` is from Daniel Hepper's
[font8x8](https://github.com/dhepper/font8x8), which is based on the IBM VGA fonts by way of
Marcel Sondaar. It is in the public domain.

### X11 misc-fixed fonts (public domain)

`firmware/picocalc_studio/src/sdk/uifont.h` (`FONT_6X10`, `FONT_7X13`) was generated from the 6x10
and 7x13 fonts as they appear in [FabGL](https://github.com/fdivitto/FabGL) (`font_6x10.h`,
`font_7x13.h`). These are the X11 "misc-fixed" bitmap fonts, which are in the public domain. Only
the glyph data was taken, not FabGL's code.

### PicoCalc hardware layer

The P4 and RP2350 hardware layer (`src/hal/picocalc_p4/`, `src/hal/picocalc_rp2350/`) adapts the
display transport, keyboard protocol and timings, audio output and loader hand-off from the
author's own PicoCalc projects: pico-386, emu16 and emu8. The pin maps follow PicoMultiLoader and
emu8.

## Linked into the firmware

The firmware binaries (`.bin`, `.uf2`, and every app F5 exports) are built with, and contain
parts of:

| Component | Used for | License |
|---|---|---|
| [arduino-esp32](https://github.com/espressif/arduino-esp32) | ESP32-P4 Arduino core | LGPL-2.1 |
| [ESP-IDF](https://github.com/espressif/esp-idf) | ESP32-P4 SDK, drivers, FreeRTOS port | Apache-2.0 |
| [arduino-pico](https://github.com/earlephilhower/arduino-pico) 6.1.1 | RP2350 Arduino core, SDFS, PWMAudio | LGPL-2.1 |
| [pico-sdk](https://github.com/raspberrypi/pico-sdk) | RP2350 SDK (through arduino-pico) | BSD-3-Clause |
| [SdFat](https://github.com/greiman/SdFat) | SD card file system on the RP2350 (through arduino-pico) | MIT |
| [FreeRTOS](https://www.freertos.org/) | tasks on both boards | MIT |
| [newlib](https://sourceware.org/newlib/) | C library of the firmware, which programs also call (stdio, string, math) | BSD-style and similar permissive licenses; see newlib's `COPYING.NEWLIB` |
| libgcc | soft-float and 64-bit arithmetic helpers | GPL-3.0 with the GCC Runtime Library Exception |

The C headers in `sdcard/picocalc-studio/include/` were written for this project. They declare
the parts of the firmware's newlib that programs may call.

## Tools (not distributed)

Building and testing use, without distributing them: arduino-cli, the Espressif and
pqt-gcc-riscv RISC-V GCC toolchains, MinGW-w64 GCC, Python, Pillow (for `tools/host/shots`) and
esptool (to validate exported images). PicoMultiLoader is the SD-card boot menu that runs
PicoCalc Studio and its exported apps. It is a separate project, and none of its code is included
here.
