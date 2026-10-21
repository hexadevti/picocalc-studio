// export.cpp - see export.h. The image math is in image.c (shared with the host tool pcexport).
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "export.h"
#include "image.h"
#include "../rt/runner.h"
#include "../sdk/picocalc.h"
#include "../sdk/sdk_internal.h"
#include "../hal/hal.h"

// The slot. Lives in .flash.text via the "*(.text.*)" input pattern, so it is inside the image
// (and its SHA-256) and readable through the flash mapping. Phase 0 spike (b) validated this.
asm(".section .text.pc_slot,\"ax\",@progbits\n"
    ".balign 4\n"   // more than the default .flash.text alignment breaks IDF's dummy rodata section
    ".globl pc_slot\n"
    "pc_slot:\n"
    ".fill 0x40000, 1, 0xff\n"
    ".previous\n");
extern "C" const uint8_t pc_slot[];
static_assert(SLOT_SIZE == 0x40000, "SLOT_SIZE must match the .fill above");
static_assert(sizeof(SlotHeader) == 64, "SlotHeader layout");

static long fail(char *err, int errlen, const char *fmt, const char *arg = nullptr) {
  snprintf(err, errlen, fmt, arg);
  return -1;
}

// ---- export ----

struct Ctx {
  const esp_partition_t *part;
  FILE *f;
};

static int partRead(void *c, uint32_t off, void *buf, uint32_t n) {
  return esp_partition_read(((Ctx *)c)->part, off, buf, n) == ESP_OK ? 0 : -1;
}

static int fileWrite(void *c, const void *buf, uint32_t n) {
  return fwrite(buf, 1, n, ((Ctx *)c)->f) == n ? 0 : -1;
}

static int fileRead(void *c, uint32_t off, void *buf, uint32_t n) {
  FILE *f = ((Ctx *)c)->f;
  return fseek(f, off, SEEK_SET) || fread(buf, 1, n, f) != n ? -1 : 0;
}

long ex_export(const void *pcx, size_t size, const char *name, const char *path, char *err, int errlen) {
  uint8_t *slot = (uint8_t *)heap_caps_malloc(SLOT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!slot) return fail(err, errlen, "out of memory");
  if (slot_build(slot, pcx, size, name, SLOT_AUTORUN)) {
    free(slot);
    return fail(err, errlen, "the program is too big for the export slot (256 KB)");
  }
  char dir[192], tmp[200];
  snprintf(dir, sizeof dir, "%s", path);
  if (char *s = strrchr(dir, '/')) *s = 0;
  if (mkdir(dir, 0777) && errno != EEXIST) {
    free(slot);
    return fail(err, errlen, "cannot create %s", dir);
  }
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  Ctx c = {esp_ota_get_running_partition(), fopen(tmp, "wb")};
  if (!c.f) {
    free(slot);
    return fail(err, errlen, "cannot create %s", tmp);
  }
  setvbuf(c.f, nullptr, _IOFBF, 16 * 1024);
  const ImgPatch patch = {(uint32_t)(uintptr_t)pc_slot, slot, SLOT_SIZE};
  long n = img_export(partRead, fileWrite, &c, &patch, 1, name, err, errlen);
  free(slot);
  if (fclose(c.f) && n >= 0) n = fail(err, errlen, "write error on the SD card");
  if (n >= 0) {   // read it back: the loader would refuse a bad image, better to know now
    char why[100];
    c.f = fopen(tmp, "rb");
    const long v = c.f ? img_verify(fileRead, &c, (uint32_t)n, why, sizeof why) : -1;
    if (c.f) fclose(c.f);
    if (v != n) n = fail(err, errlen, "the written file is bad: %s", c.f ? why : "cannot reopen it");
  }
  if (n >= 0) {
    remove(path);
    if (rename(tmp, path)) n = fail(err, errlen, "cannot rename the file to %s", path);
  }
  if (n < 0) remove(tmp);
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
  const void *src = slot_pcx(pc_slot, &size, &name);
  // A RAM copy: the loader writes the module to flash, and the slot is flash too.
  uint8_t *image = (uint8_t *)heap_caps_malloc(size ? size : 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!image) {
    endScreen("Out of memory loading the program.");
    hal_reboot();
  }
  memcpy(image, src, size);
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
