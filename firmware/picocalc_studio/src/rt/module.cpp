// module.cpp - .pcx loader: code in the ota_0 tail (mapped as instructions), data in RAM.
// The technique is the one firmware/spikes/spike_p0 validated on the device.
#include <string.h>
#include <stdio.h>
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_flash.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "module.h"
#include "exports.h"
#include "../hal/hal.h"

// ---- flash write guard ----
// IDF aborts on any write inside the running partition. Wrap the default chip's region_protected
// hook so exactly [end of image, end of partition) is writable and IDF decides everything else.

static const esp_flash_os_functions_t *s_origOs;
static esp_flash_os_functions_t s_os;
static uint32_t s_allowLo, s_allowHi;

static esp_err_t IRAM_ATTR tailRegionProtected(void *arg, size_t addr, size_t size) {
  if (addr >= s_allowLo && addr + size <= s_allowHi) return ESP_OK;
  return s_origOs->region_protected ? s_origOs->region_protected(arg, addr, size) : ESP_OK;
}

static const esp_partition_t *s_part;
static uint32_t s_tail;   // partition offset of the code area (64 KB aligned)

static bool initTail(char *err, int errlen) {
  if (s_part) return true;
  const esp_partition_t *p = esp_ota_get_running_partition();
  esp_partition_pos_t pos = {p->address, p->size};
  esp_image_metadata_t meta;
  if (esp_image_get_metadata(&pos, &meta) != ESP_OK) {
    snprintf(err, errlen, "cannot read the firmware image size");
    return false;
  }
  s_tail = (meta.image_len + 0xFFFF) & ~0xFFFFu;
  if (s_tail + 0x10000 > p->size) {
    snprintf(err, errlen, "no room for code after the firmware image");
    return false;
  }
  s_allowLo = p->address + s_tail;
  s_allowHi = p->address + p->size;
  s_origOs = esp_flash_default_chip->os_func;
  s_os = *s_origOs;
  s_os.region_protected = tailRegionProtected;
  esp_flash_default_chip->os_func = &s_os;
  s_part = p;
  return true;
}

// ---- symbols ----

static void *(*s_resolveOverride)(const char *name);

static void *resolveSym(const char *name) {
  void *a = s_resolveOverride ? s_resolveOverride(name) : nullptr;
  return a ? a : rt_find_export(name);
}

// ---- crash record ----
// The whole module image is also written to flash after the code, and this record (noinit RAM:
// it survives a panic reset) says where. After a crash the next boot maps the PC to file:line.

#define CRASH_MAGIC 0x50435852u   // "PCXR"
struct CrashRecord {
  uint32_t magic;
  uint32_t text_addr, text_size;
  uint32_t image_off, image_size;   // partition offset of the image copy
};
static __NOINIT_ATTR CrashRecord s_rec;

// True if the flash at partition offset `off` already holds `data`: an exported program loads the
// same module at every boot, and rerunning a program reloads the same code. Saves an erase.
static bool sameInFlash(uint32_t off, const void *data, uint32_t n) {
  uint8_t buf[512];
  for (uint32_t i = 0; i < n; i += sizeof buf) {
    const uint32_t k = n - i < sizeof buf ? n - i : sizeof buf;
    if (esp_partition_read(s_part, off + i, buf, k) != ESP_OK || memcmp(buf, (const uint8_t *)data + i, k)) return false;
  }
  return true;
}

static int fail(char *err, int errlen, const char *msg) {
  snprintf(err, errlen, "%s", msg);
  return -1;
}

int rt_module_load(const void *image, size_t size, RtModule *m, void *(*resolve)(const char *name),
                   char *err, int errlen) {
  memset(m, 0, sizeof *m);
  if (pcx_parse(image, size, &m->view, err, errlen)) return -1;
  if (!initTail(err, errlen)) return -1;
  const PcxHeader *h = m->view.h;
  const uint32_t mapSize = (h->text_size + 0xFFFF) & ~0xFFFFu;
  const uint32_t imageOff = (h->text_size + 0xFFF) & ~0xFFFu;
  const uint32_t eraseSize = imageOff + ((size + 0xFFF) & ~0xFFFu);
  if (s_tail + mapSize > s_part->size || s_tail + eraseSize > s_part->size)
    return fail(err, errlen, "program code too big");
  s_rec.magic = 0;

  const uint32_t ramSize = h->data_size + h->bss_size;
  const uint32_t align = h->ram_align < 16 ? 16 : h->ram_align;
  m->ram = (uint8_t *)heap_caps_aligned_alloc(align, ramSize ? ramSize : 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!m->ram) m->ram = (uint8_t *)heap_caps_aligned_alloc(align, ramSize ? ramSize : 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uint8_t *buf = (uint8_t *)heap_caps_malloc(h->text_size ? h->text_size : 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!m->ram || !buf) {
    free(buf);
    rt_module_unload(m);
    return fail(err, errlen, "out of memory loading the program");
  }
  s_resolveOverride = resolve;

  // The code is relocated for the address it will run at, which is only known once the flash is
  // mapped. Map once to learn it, write, map again; the MMU hands out the same slot for the same
  // size, but check and redo the relocation if it doesn't.
  const void *ptr = nullptr;
  esp_partition_mmap_handle_t hnd;
  if (mapSize && esp_partition_mmap(s_part, s_tail, mapSize, ESP_PARTITION_MMAP_INST, &ptr, &hnd) != ESP_OK) {
    free(buf);
    rt_module_unload(m);
    return fail(err, errlen, "cannot map the code area");
  }
  if (mapSize) esp_partition_munmap(hnd);
  for (int attempt = 0;; attempt++) {
    memcpy(buf, m->view.text, h->text_size);
    if (pcx_relocate(&m->view, buf, (uint32_t)(uintptr_t)ptr, m->ram, resolveSym, err, errlen)) {
      free(buf);
      rt_module_unload(m);
      return -1;
    }
    if (!mapSize) break;
    const bool same = sameInFlash(s_tail, buf, h->text_size) && sameInFlash(s_tail + imageOff, image, size);
    if (!same && (esp_partition_erase_range(s_part, s_tail, eraseSize) != ESP_OK ||
                  esp_partition_write(s_part, s_tail, buf, h->text_size) != ESP_OK ||
                  esp_partition_write(s_part, s_tail + imageOff, image, size) != ESP_OK)) {
      free(buf);
      rt_module_unload(m);
      return fail(err, errlen, "flash write failed");
    }
    const void *now;
    if (esp_partition_mmap(s_part, s_tail, mapSize, ESP_PARTITION_MMAP_INST, &now, &hnd) != ESP_OK) {
      free(buf);
      rt_module_unload(m);
      return fail(err, errlen, "cannot map the code area");
    }
    if (now == ptr) break;
    esp_partition_munmap(hnd);
    ptr = now;
    if (attempt == 2) {
      free(buf);
      rt_module_unload(m);
      return fail(err, errlen, "code mapping address keeps moving");
    }
  }
  free(buf);
  __asm__ volatile("fence.i" ::: "memory");
  m->text = (uint8_t *)ptr;
  m->mmap_handle = mapSize ? (uint32_t)hnd : 0;
  if (h->entry_main != PCX_NO_ENTRY) m->main = (int (*)(int, char **))(void *)(m->text + h->entry_main);
  if (h->entry_setup != PCX_NO_ENTRY) m->setup = (void (*)(void))(void *)(m->text + h->entry_setup);
  if (h->entry_loop != PCX_NO_ENTRY) m->loop = (void (*)(void))(void *)(m->text + h->entry_loop);
  s_rec.text_addr = (uint32_t)(uintptr_t)m->text;
  s_rec.text_size = h->text_size;
  s_rec.image_off = s_tail + imageOff;
  s_rec.image_size = size;
  s_rec.magic = CRASH_MAGIC;
  return 0;
}

void rt_module_unload(RtModule *m) {
  s_rec.magic = 0;
  if (m->mmap_handle) esp_partition_munmap((esp_partition_mmap_handle_t)m->mmap_handle);
  heap_caps_free(m->ram);
  memset(m, 0, sizeof *m);
}

bool rt_module_where(const RtModule *m, uint32_t pc, const char **file, int *line) {
  if (!m->text || pc < (uint32_t)(uintptr_t)m->text || pc >= (uint32_t)(uintptr_t)m->text + m->view.h->text_size)
    return false;
  return pcx_lookup_line(&m->view, pc - (uint32_t)(uintptr_t)m->text, file, line) != 0;
}

bool rt_module_crash_where(char *buf, size_t n) {
  if (s_rec.magic != CRASH_MAGIC) return false;
  s_rec.magic = 0;
  uint32_t pcs[16];
  const int npc = hal_crash_pcs(pcs, 16);
  if (!npc) return false;
  const esp_partition_t *p = esp_ota_get_running_partition();
  if (s_rec.image_size > 4u * 1024 * 1024 || s_rec.image_off + s_rec.image_size > p->size) return false;
  uint8_t *img = (uint8_t *)heap_caps_malloc(s_rec.image_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!img) return false;
  bool found = false;
  char err[80];
  PcxView v;
  if (esp_partition_read(p, s_rec.image_off, img, s_rec.image_size) == ESP_OK &&
      !pcx_parse(img, s_rec.image_size, &v, err, sizeof err)) {
    // The pc itself, else the innermost return address into the program (a crash inside a
    // firmware function the program called). Return addresses point after the call: look up -2.
    for (int i = 0; i < npc && !found; i++) {
      if (pcs[i] < s_rec.text_addr || pcs[i] >= s_rec.text_addr + s_rec.text_size) continue;
      const char *file;
      int line;
      uint32_t off = pcs[i] - s_rec.text_addr - (i ? 2 : 0);
      if (pcx_lookup_line(&v, off, &file, &line)) {
        snprintf(buf, n, "%s:%d%s", file, line, i ? " (in a call from there)" : "");
        found = true;
      }
    }
  }
  free(img);
  return found;
}
