#!/usr/bin/env python3
"""Prelink hook for tools/build_rp2350.ps1: arduino-pico's RP2350 linker script runs all of libgcc
and libm from RAM (~65 KB of .data), which PicoCalc Studio needs as heap (framebuffer + compiler).
Keep only libc's mem* routines in RAM; everything else runs from flash through the XIP cache.

  python rp2350_ldflash.py <build path>/memmap_default.ld
"""
import sys
from pathlib import Path

OLD = "EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:)"
NEW = "EXCLUDE_FILE(*libc.a:*lib_a-mem*.o)"

ld = Path(sys.argv[1])
text = ld.read_text()
if OLD not in text and NEW not in text:
    sys.exit(f"rp2350_ldflash: {ld}: the RAM exclusion list changed, update this script")
ld.write_text(text.replace(OLD, NEW))
