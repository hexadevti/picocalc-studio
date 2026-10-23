// module.cpp - the .pcx loader on the RP2350 (see src/rt/module.h): SRAM is executable here, so
// the code is relocated in place and run from there; no flash involved. An image in SRAM keeps
// its code where it is (relocating again rewrites the same fields, so it can be run again);
// one in flash (an exported program's slot) gets a heap copy.
// There is no copy of the image to map a crash PC back to file:line after a reset, so
// rt_module_crash_where() never finds one; hal_crash_report() still gives the raw PCs.
#ifdef ARDUINO_ARCH_RP2040
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include "hardware/regs/addressmap.h"
#include "../../rt/module.h"
#include "../../rt/exports.h"

static void *(*s_resolveOverride)(const char *name);

static void *resolveSym(const char *name) {
  void *a = s_resolveOverride ? s_resolveOverride(name) : nullptr;
  return a ? a : rt_find_export(name);
}

static int fail(char *err, int errlen, const char *msg) {
  snprintf(err, errlen, "%s", msg);
  return -1;
}

int rt_module_load(const void *image, size_t size, RtModule *m, void *(*resolve)(const char *name),
                   char *err, int errlen) {
  memset(m, 0, sizeof *m);
  if (pcx_parse(image, size, &m->view, err, errlen)) return -1;
  const PcxHeader *h = m->view.h;
  const uint32_t ramSize = h->data_size + h->bss_size;
  const uint32_t align = h->ram_align < 16 ? 16 : h->ram_align;
  m->ram = (uint8_t *)memalign(align, ramSize ? ramSize : 4);
  const uintptr_t t = (uintptr_t)m->view.text;
  if (t >= SRAM_BASE && t < SRAM_END && !(t & 3)) m->text = (uint8_t *)t;
  else m->text = (uint8_t *)memalign(16, h->text_size ? h->text_size : 4);
  if (!m->ram || !m->text) {
    rt_module_unload(m);
    return fail(err, errlen, "out of memory loading the program");
  }
  s_resolveOverride = resolve;
  if (m->text != m->view.text) memcpy(m->text, m->view.text, h->text_size);
  if (pcx_relocate(&m->view, m->text, (uint32_t)(uintptr_t)m->text, m->ram, resolveSym, err, errlen)) {
    rt_module_unload(m);
    return -1;
  }
  __asm__ volatile("fence.i" ::: "memory");
  if (h->entry_main != PCX_NO_ENTRY) m->main = (int (*)(int, char **))(void *)(m->text + h->entry_main);
  if (h->entry_setup != PCX_NO_ENTRY) m->setup = (void (*)(void))(void *)(m->text + h->entry_setup);
  if (h->entry_loop != PCX_NO_ENTRY) m->loop = (void (*)(void))(void *)(m->text + h->entry_loop);
  return 0;
}

void rt_module_unload(RtModule *m) {
  if (m->text != m->view.text) free(m->text);
  free(m->ram);
  memset(m, 0, sizeof *m);
}

bool rt_module_where(const RtModule *m, uint32_t pc, const char **file, int *line) {
  if (!m->text || pc < (uint32_t)(uintptr_t)m->text || pc >= (uint32_t)(uintptr_t)m->text + m->view.h->text_size)
    return false;
  return pcx_lookup_line(&m->view, pc - (uint32_t)(uintptr_t)m->text, file, line) != 0;
}

bool rt_module_crash_where(char *, size_t) { return false; }

#endif  // ARDUINO_ARCH_RP2040
