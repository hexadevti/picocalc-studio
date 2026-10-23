// export.cpp - program export (src/export/export.h) on the RP2350. The exported firmware is a UF2
// for PicoMultiLoader: a copy of the running image, read back from flash (0x10000000 up to
// __flash_binary_end, the boot stub included), with the program in the export slot. The image
// carries no hash or signature, so nothing else changes. The loader menu shows the file name
// (the image has no binary_info program name), so the file is named after the project.
#ifdef ARDUINO_ARCH_RP2040
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "../../export/export.h"
#include "../../export/image.h"
#include "../../rt/runner.h"
#include "../../sdk/picocalc.h"
#include "../../sdk/sdk_internal.h"
#include "../hal.h"

// The slot, in .rodata (flash), inside the image the loader installs.
asm(".section .flashdata.pc_slot,\"a\",@progbits\n"
    ".balign 256\n"
    ".globl pc_slot\n"
    "pc_slot:\n"
    ".fill 0x40000, 1, 0xff\n"
    ".previous\n");
extern "C" const uint8_t pc_slot[];
extern "C" const uint8_t __flash_binary_start[], __flash_binary_end[];
static_assert(SLOT_SIZE == 0x40000, "SLOT_SIZE must match the .fill above");
static_assert(sizeof(SlotHeader) == 64, "SlotHeader layout");

static long fail(char *err, int errlen, const char *fmt, const char *arg = nullptr) {
  snprintf(err, errlen, fmt, arg);
  return -1;
}

// ---- export ----

namespace {

const uint32_t UF2_MAGIC0 = 0x0A324655, UF2_MAGIC1 = 0x9E5D5157, UF2_MAGIC_END = 0x0AB16F30;
const uint32_t UF2_FLAG_FAMILY = 0x2000, FAMILY_RP2350_RISCV = 0xe48bff5a;
const uint32_t PAYLOAD = 256;

struct Uf2Block {
  uint32_t magic0, magic1, flags, addr, size, blockNo, numBlocks, family;
  uint8_t data[476];
  uint32_t magicEnd;
};
static_assert(sizeof(Uf2Block) == 512, "UF2 block layout");

// The exported image: the running one with the slot holding hdr and pcx (as slot_build() fills it).
struct Source {
  SlotHeader hdr;
  const uint8_t *pcx;
  uint32_t base, end, numBlocks;

  // The 256 bytes at block i.
  void payload(uint32_t i, uint8_t *out) const {
    const uint32_t a = base + i * PAYLOAD, n = a + PAYLOAD <= end ? PAYLOAD : end - a;
    memset(out, 0xFF, PAYLOAD);
    memcpy(out, (const void *)(uintptr_t)a, n);
    const uint32_t slot = (uint32_t)(uintptr_t)pc_slot;
    for (uint32_t k = 0; k < PAYLOAD; k++) {
      const uint32_t off = a + k - slot;   // wraps to a huge value below the slot
      if (off >= SLOT_SIZE) continue;
      if (off < sizeof hdr) out[k] = ((const uint8_t *)&hdr)[off];
      else if (off - hdr.pcx_offset < hdr.pcx_size) out[k] = pcx[off - hdr.pcx_offset];
      else out[k] = 0xFF;
    }
  }

  void block(uint32_t i, Uf2Block *b) const {
    *b = {UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY, base + i * PAYLOAD, PAYLOAD, i, numBlocks,
          FAMILY_RP2350_RISCV, {}, UF2_MAGIC_END};
    payload(i, b->data);
  }
};

}  // namespace

long ex_export(const void *pcx, size_t size, const char *name, const char *path, char *err, int errlen) {
  Source src;
  if (size > SLOT_SIZE - sizeof src.hdr)
    return fail(err, errlen, "the program is too big for the export slot (256 KB)");
  src.hdr = {SLOT_MAGIC, SLOT_VERSION, SLOT_AUTORUN, sizeof src.hdr, (uint32_t)size, {}};
  snprintf(src.hdr.name, sizeof src.hdr.name, "%s", name ? name : "");
  src.pcx = (const uint8_t *)pcx;
  src.base = (uint32_t)(uintptr_t)__flash_binary_start;
  src.end = (uint32_t)(uintptr_t)__flash_binary_end;
  src.numBlocks = (src.end - src.base + PAYLOAD - 1) / PAYLOAD;

  // PicoMultiLoader lists .uf2 files only.
  char out[200], dir[200], tmp[208];
  snprintf(out, sizeof out, "%s", path);
  if (char *dot = strrchr(out, '.')) if (!strchr(dot, '/')) *dot = 0;
  if (strlen(out) + 4 >= sizeof out) return fail(err, errlen, "the path is too long");
  strcat(out, ".uf2");
  snprintf(dir, sizeof dir, "%s", out);
  if (char *s = strrchr(dir, '/')) *s = 0;
  if (mkdir(dir, 0777) && errno != EEXIST) return fail(err, errlen, "cannot create %s", dir);
  snprintf(tmp, sizeof tmp, "%s.tmp", out);

  // Not static: a build of a big program needs every KB of RAM the IDE leaves.
  Uf2Block *blk = (Uf2Block *)malloc(2 * sizeof(Uf2Block));
  if (!blk) return fail(err, errlen, "out of memory");
  Uf2Block &b = blk[0], &r = blk[1];
  FILE *f = fopen(tmp, "wb");
  if (!f) {
    free(blk);
    return fail(err, errlen, "cannot create %s", tmp);
  }
  setvbuf(f, nullptr, _IOFBF, 8 * 1024);
  long n = 0;
  for (uint32_t i = 0; i < src.numBlocks && n >= 0; i++) {
    src.block(i, &b);
    if (fwrite(&b, sizeof b, 1, f) != 1) n = fail(err, errlen, "write error on the SD card");
  }
  if (fclose(f) && n >= 0) n = fail(err, errlen, "write error on the SD card");
  if (n >= 0) {   // read it back: the loader would refuse a bad file, better to know now
    if (!(f = fopen(tmp, "rb"))) {
      n = fail(err, errlen, "the written file is bad: cannot reopen it");
    } else {
      for (uint32_t i = 0; i < src.numBlocks && n >= 0; i++) {
        src.block(i, &b);
        if (fread(&r, sizeof r, 1, f) != 1 || memcmp(&b, &r, sizeof b))
          n = fail(err, errlen, "the written file is bad: it differs from the image");
      }
      if (n >= 0 && fgetc(f) != EOF) n = fail(err, errlen, "the written file is bad: too long");
      fclose(f);
    }
  }
  if (n >= 0) {
    remove(out);   // FAT rename() won't replace a file
    if (rename(tmp, out)) n = fail(err, errlen, "cannot rename the file to %s", out);
    else n = (long)src.numBlocks * (long)sizeof b;
  }
  if (n < 0) remove(tmp);
  free(blk);
  return n;
}

// ---- running an exported program ----

bool ex_is_app() {
  return slot_pcx(pc_slot, nullptr, nullptr) && (((const SlotHeader *)(const void *)pc_slot)->flags & SLOT_AUTORUN);
}

// Shows why the program ended. Returns when a key asks to run it again; Esc goes to the loader.
static void endScreen(const char *msg) {
  snd_stop();
  gfx_clip(0, 0, SCREEN_W, SCREEN_H);
  gfx_font(FONT_6X10);
  gfx_fill_rect(0, SCREEN_H - 24, SCREEN_W, 24, DARKGRAY);
  gfx_text(4, SCREEN_H - 20, msg, WHITE);
  gfx_text(4, SCREEN_H - 10, "Any key: run again   Esc: loader", LIGHTGRAY);
  gfx_flush();
  kbd_flush();
  HalKey k;
  for (;;) {
    while (hal_key_poll(&k)) {
      if (k.state != HAL_KEY_PRESSED) continue;
      if (k.code == KEY_ESC) hal_reboot_to_loader();
      if (k.code != KEY_BREAK) return;
    }
    hal_delay(10);
  }
}

void ex_run_app() {
  size_t size;
  const char *name;
  // Run from the slot itself: the loader copies the code to RAM (module.cpp), flash stays as is.
  const void *image = slot_pcx(pc_slot, &size, &name);
  char argv0[sizeof SlotHeader::name];
  snprintf(argv0, sizeof argv0, "%s", name);
  char *argv[2] = {argv0, nullptr};
  for (;;) {
    RtRunOptions opt = {1, argv, nullptr, true, 0};
    RtRunResult r;
    char msg[60];
    hal_breadcrumb("app %s", argv0);
    if (rt_run(image, size, &opt, &r)) {
      hal_log("can't load %s: %s", argv0, r.err);
      snprintf(msg, sizeof msg, "Can't load: %s", r.err);
    } else {
      hal_log("app %s: %s, status %d", argv0, rt_end_name(r.end), r.status);
      switch (r.end) {
        case RT_END_RETURN: case RT_END_EXIT:
          if (r.status) snprintf(msg, sizeof msg, "%s ended, status %d.", argv0, r.status);
          else snprintf(msg, sizeof msg, "%s ended.", argv0);
          break;
        case RT_END_BREAK: snprintf(msg, sizeof msg, "%s stopped (Break).", argv0); break;
        case RT_END_KILLED: snprintf(msg, sizeof msg, "%s stopped by force.", argv0); break;
        case RT_END_ABORT: snprintf(msg, sizeof msg, "%s called abort().", argv0); break;
        case RT_END_BAD_FREE: snprintf(msg, sizeof msg, "%s freed a bad pointer.", argv0); break;
        case RT_END_TIMEOUT: snprintf(msg, sizeof msg, "%s timed out.", argv0); break;
      }
    }
    endScreen(msg);
  }
}

#endif  // ARDUINO_ARCH_RP2040
