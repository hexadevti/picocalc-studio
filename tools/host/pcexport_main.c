// pcexport_main.c - host driver for the firmware exporter core (src/export/image.c), so its image
// math can be checked on the PC against tools/host/esp_image.py and esptool.
//
//   pcexport <firmware.bin> --slot 0x... --pcx prog.pcx [--name n] -o out.bin
//   pcexport verify <image.bin>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"

typedef struct {
  const uint8_t *src;
  size_t src_len;
  FILE *out;
} Ctx;

static int rd(void *c, uint32_t off, void *buf, uint32_t n) {
  Ctx *x = (Ctx *)c;
  if (off > x->src_len || n > x->src_len - off) return -1;
  memcpy(buf, x->src + off, n);
  return 0;
}

static int wr(void *c, const void *buf, uint32_t n) { return fwrite(buf, 1, n, ((Ctx *)c)->out) == n ? 0 : -1; }

static uint8_t *load(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  uint8_t *b;
  long n;
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  b = (uint8_t *)malloc(n ? (size_t)n : 1);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "%s: read error\n", path); exit(1); }
  fclose(f);
  *len = (size_t)n;
  return b;
}

int main(int argc, char **argv) {
  char err[160];
  Ctx c = {0};
  const char *image = NULL, *pcx = NULL, *name = NULL, *out = NULL;
  unsigned long slot = 0;
  int i;
  if (argc == 3 && !strcmp(argv[1], "verify")) {
    long n;
    c.src = load(argv[2], &c.src_len);
    n = img_verify(rd, &c, (uint32_t)c.src_len, err, sizeof err);
    if (n < 0) { printf("INVALID: %s\n", err); return 1; }
    printf("valid image, %ld bytes%s\n", n, (size_t)n == c.src_len ? "" : " (file has extra bytes)");
    return 0;
  }
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--slot") && i + 1 < argc) slot = strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--pcx") && i + 1 < argc) pcx = argv[++i];
    else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!image) image = argv[i];
    else { fprintf(stderr, "unexpected argument %s\n", argv[i]); return 2; }
  }
  if (!image || !pcx || !out || !slot) {
    fprintf(stderr, "usage: pcexport <firmware.bin> --slot 0xADDR --pcx prog.pcx [--name n] -o out.bin\n"
                    "       pcexport verify <image.bin>\n");
    return 2;
  }
  {
    size_t pcx_len;
    uint8_t *p = load(pcx, &pcx_len), *s = (uint8_t *)malloc(SLOT_SIZE);
    ImgPatch patch;
    long n;
    if (slot_build(s, p, pcx_len, name, SLOT_AUTORUN)) { fprintf(stderr, "program too big for the slot\n"); return 1; }
    c.src = load(image, &c.src_len);
    c.out = fopen(out, "wb");
    if (!c.out) { perror(out); return 1; }
    patch.vaddr = (uint32_t)slot;
    patch.data = s;
    patch.size = SLOT_SIZE;
    n = img_export(rd, wr, &c, &patch, 1, name, err, sizeof err);
    fclose(c.out);
    if (n < 0) { fprintf(stderr, "export failed: %s\n", err); remove(out); return 1; }
    printf("%s: %ld bytes\n", out, n);
  }
  return 0;
}
