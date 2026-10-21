// image.h - ESP32-P4 app image rewriting, the core of the firmware exporter (export.cpp).
//
// Copies an app image while patching bytes at virtual addresses (the PC_SLOT export slot) and
// the app descriptor's project name, recomputing the XOR checksum and the appended SHA-256 as it
// goes, so the bootloader and PicoMultiLoader accept the copy. Streams in small chunks: the image
// is never held in memory. tools/host/esp_image.py is the PC twin.
//
// Portable C99: built by the firmware and by host gcc (tools/host/pcexport_main.c).
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The export slot: a SLOT_SIZE block of 0xFF in the firmware image (export.cpp), filled by an
// export with a SlotHeader followed by the program's .pcx module.
#define SLOT_MAGIC 0x4C534350u   // "PCSL"
#define SLOT_VERSION 2           // 1 was the Phase 0 probe slot
#define SLOT_SIZE (256 * 1024)
#define SLOT_AUTORUN 1u

typedef struct {
  uint32_t magic, version;
  uint32_t flags;                // SLOT_AUTORUN
  uint32_t pcx_offset;           // from the start of the slot, 4-byte aligned
  uint32_t pcx_size;
  char name[44];                 // project name, NUL-terminated
} SlotHeader;                    // 64 bytes

// Fills slot (SLOT_SIZE bytes): header, pcx, 0xFF up to the end. Returns 0, -1 if it doesn't fit.
int slot_build(uint8_t *slot, const void *pcx, size_t pcx_size, const char *name, uint32_t flags);

// The pcx inside a slot as linked into a running image, or NULL if the slot is empty or invalid.
const void *slot_pcx(const uint8_t *slot, size_t *size, const char **name);

typedef struct {
  uint32_t vaddr;                // where the bytes live in the running firmware's address space
  const void *data;
  uint32_t size;
} ImgPatch;

// Reads n bytes at offset off of the source image; writes the next n bytes of the output.
// Both return 0 on success.
typedef int (*img_read_fn)(void *ctx, uint32_t off, void *buf, uint32_t n);
typedef int (*img_write_fn)(void *ctx, const void *buf, uint32_t n);

// Writes a copy of the source image with the patches applied (each must lie inside one
// segment) and, if name is not NULL, the app descriptor's project_name set to it. Returns the
// output size, or -1 with a message in err.
long img_export(img_read_fn rd, img_write_fn wr, void *ctx, const ImgPatch *patches, int npatches,
                const char *name, char *err, int errlen);

// Checks an image's structure, XOR checksum and SHA-256 (the bootloader's checks). Returns the
// image length, or -1 with a message in err. size is the number of bytes available to read.
long img_verify(img_read_fn rd, void *ctx, uint32_t size, char *err, int errlen);

#ifdef __cplusplus
}
#endif
