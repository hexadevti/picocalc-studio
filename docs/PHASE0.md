# Phase 0: feasibility spikes

Two questions gate the design:

- **(a) Run.** Can freshly compiled code run from flash?
- **(b) Export.** Can a program be baked into a copy of the firmware image that PicoMultiLoader will flash and boot?

## Findings so far (desk check + PC-side)

| # | Finding | Consequence |
|---|---|---|
| 1 | IDF refuses **any** flash write inside the running partition. `esp_partition_main_flash_region_safe()` covers the whole `ota_0`, free tail included, and the Arduino sdkconfig has `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS=y`, so a write would `abort()`. | The runtime wraps `esp_flash_default_chip->os_func->region_protected`. The wrapper allows exactly `[image end, partition end)` and passes everything else to IDF's check. The image, bootloader and partition table stay protected. |
| 2 | With `PMP_IDRAM_SPLIT`, the CPU's PMP fences the firmware's flash ranges: code is RX up to `_instruction_reserved_end`, rodata is R up to `_rodata_reserved_end`. The PSRAM heap is RW and **not** executable. Pages mmap'd later sit above those ranges, where no PMP entry matches, so machine mode can execute them. | Running code from an `esp_partition_mmap(INST)` mapping should work. The spike checks the mapping address. |
| 3 | A 1 MB 0xFF block in section `.text.pc_slot` links into `.flash.text`. It lands at `0x40000020`, the start of irom, in segment 4. Its alignment must be ≤ 4: a larger alignment moves `.flash.text` and breaks IDF's `.flash_rodata_dummy` (VMA overlap link error). | The export slot works. Compiled code needs only 2/4-byte alignment. |
| 4 | `tools/host/esp_image.py` rebuilds a stock image byte-for-byte. After patching the slot, **esptool 5.3 independently reports checksum and SHA-256 valid**. | The exporter's image math is right. The firmware port (`src/export`) does the same steps. |
| 5 | The Arduino `postv3` build writes chip rev min v0.0 / max 0xFFFF. PicoMultiLoader's check (`image_flash.c:136`) accepts that. | No header fix-up needed. |

## Device results (2026-10-20)

Spike (a): 11/11 PASS on two builds.
- The tail mapping lands right at the end of drom (0x40170000) and runs. Finding 2 is confirmed.
- The call into the module and the callback into the firmware both work: `probe` returned 1026.
- After a rewrite and remap, the new code runs (2032); the cache was not stale.
- A rewrite while the region is still mapped also ran the new code. IDF invalidates the cache on writes.
- Erasing and writing 64 KB takes 70 to 130 ms. A mapping plus 10 calls takes 51 us.
- A 200 KB internal-RAM framebuffer cuts internal heap from 435 KB to 231 KB. The SDK framebuffer should go in PSRAM, with DMA bounce buffers as in pico-386.
- `esp_app_desc.project_name` is `arduino-lib-builder`. The exporter must set it, or the loader menu shows that name.

Spike (b): PASS. PicoMultiLoader flashed and booted `spike_p0_slot.bin`, which was patched on the PC with its checksum and SHA-256 recomputed. The sketch reported `slot: PATCHED` and its probe ran from the slot. Export by patching the in-image slot works.

**Phase 0 gate: passed.** Both the run path (ota_0 tail plus mmap) and the export path (in-image slot) work on the device.

## Running it on the device (PicoCalc + ESP32-P4)

```
powershell -File tools\build.ps1 firmware\spikes\spike_p0 -ProbeSlot
```

This produces:
- `build\spike_p0\spike_p0.bin` (empty slot)
- `build\spike_p0\spike_p0_slot.bin` (probe patched into the slot)

Copy both to the SD card. **Do not** use `arduino-cli upload`: it would overwrite the loader.

1. In PicoMultiLoader, flash `spike_p0.bin`. It runs spike (a), then reports the empty slot.
2. Back to the loader (hold Up/F1/F5 while resetting). Flash `spike_p0_slot.bin`.
3. Results show on the LCD (green PASS, red FAIL) and are appended line by line to `/sd/picocalc_p0.txt`, ending in `=== N passed, 0 failed ===`. If a step crashes, the app restarts, shows `CRASH previous run died in: <step>` and skips that spike. Power-cycle to retry it.

Expected key lines:

```
PASS  write guard still protects the image itself
PASS  erase + write v1 into tail
PASS  esp_partition_mmap(INST)
PASS  mapping lies above the PMP-fenced firmware ranges
PASS  call into flash code + callback into firmware          probe = 1026
PASS  remap after rewrite runs the new code (no stale cache) probe = 2032
INFO  rewrite while mapped: ...                              informational only
PASS  empty slot is all 0xFF                                 (run 1)
PASS  patched image boots and runs the slot code             (run 2, probe = 1026)
```

If the call into the mapping crashes (`Instruction access fault`), finding 2 is wrong. The export slot is **not** a safe fallback: writing it changes the image, and the bootloader checks the SHA-256 at every boot. The next options would be:
- add a PMP entry at runtime marking the mapped window RX (the IDF entries are locked, but spare entries may remain);
- or make every run an export: write the `.bin` and have PicoMultiLoader flash it. This takes seconds per run, but uses only proven paths.
