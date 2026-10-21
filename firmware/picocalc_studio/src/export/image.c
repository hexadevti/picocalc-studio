// image.c - see image.h. Image layout (esp_image_format.h):
//   header (24 bytes; segment count at 1, hash_appended at 23)
//   segments: {load_addr, length} + length bytes, segment_count times
//   zero padding so that the checksum byte ends a 16-byte block
//   checksum: 0xEF XOR every segment data byte
//   SHA-256 of everything above, if hash_appended
#include <stdio.h>
#include <string.h>
#include "image.h"

#define IMAGE_MAGIC 0xE9
#define HDR_LEN 24
#define MAX_SEGMENTS 16
#define CHUNK 4096
#define APP_DESC_MAGIC 0xABCD5432u   // esp_app_desc_t, at the start of the first rodata segment
#define APP_DESC_NAME 48             // offsetof(esp_app_desc_t, project_name)
#define APP_DESC_NAME_LEN 32

static int fail(char *err, int errlen, const char *msg) {
  if (err && errlen > 0) snprintf(err, (size_t)errlen, "%s", msg);
  return -1;
}

// ---- SHA-256 (FIPS 180-4) ----

typedef struct {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  uint32_t n;
} Sha256;

static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha256 *s, const uint8_t *p) {
  uint32_t w[64], a, b, c, d, e, f, g, h, i;
  for (i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
  for (i = 0; i < 64; i++) {
    uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(Sha256 *s) {
  static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, h0, sizeof h0);
  s->len = 0;
  s->n = 0;
}

static void sha_update(Sha256 *s, const void *data, uint32_t n) {
  const uint8_t *p = (const uint8_t *)data;
  s->len += n;
  while (n) {
    uint32_t k = 64 - s->n < n ? 64 - s->n : n;
    if (!s->n && n >= 64) { sha_block(s, p); p += 64; n -= 64; continue; }
    memcpy(s->buf + s->n, p, k);
    s->n += k; p += k; n -= k;
    if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
  }
}

static void sha_final(Sha256 *s, uint8_t out[32]) {
  const uint64_t bits = s->len * 8;
  uint8_t pad[72] = {0x80};
  const uint32_t padLen = (s->n < 56 ? 56 : 120) - s->n;
  int i;
  for (i = 0; i < 8; i++) pad[padLen + i] = (uint8_t)(bits >> (56 - 8 * i));
  sha_update(s, pad, padLen + 8);
  for (i = 0; i < 32; i++) out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

// ---- slot ----

int slot_build(uint8_t *slot, const void *pcx, size_t pcx_size, const char *name, uint32_t flags) {
  SlotHeader h;
  if (pcx_size > SLOT_SIZE - sizeof h) return -1;
  memset(&h, 0, sizeof h);
  h.magic = SLOT_MAGIC;
  h.version = SLOT_VERSION;
  h.flags = flags;
  h.pcx_offset = sizeof h;
  h.pcx_size = (uint32_t)pcx_size;
  snprintf(h.name, sizeof h.name, "%s", name ? name : "");
  memset(slot, 0xFF, SLOT_SIZE);
  memcpy(slot, &h, sizeof h);
  memcpy(slot + sizeof h, pcx, pcx_size);
  return 0;
}

const void *slot_pcx(const uint8_t *slot, size_t *size, const char **name) {
  SlotHeader h;
  memcpy(&h, slot, sizeof h);
  if (h.magic != SLOT_MAGIC || h.version != SLOT_VERSION || (h.pcx_offset & 3u) ||
      h.pcx_offset < sizeof h || h.pcx_offset > SLOT_SIZE || h.pcx_size > SLOT_SIZE - h.pcx_offset ||
      memchr(h.name, 0, sizeof h.name) == NULL)
    return NULL;
  if (size) *size = h.pcx_size;
  if (name) *name = ((const SlotHeader *)(const void *)slot)->name;
  return slot + h.pcx_offset;
}

// ---- image ----

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// Applies the patches overlapping [vaddr, vaddr + n) to buf; counts the bytes patched.
static void apply(uint8_t *buf, uint32_t vaddr, uint32_t n, const ImgPatch *p, int np, uint32_t *done) {
  int i;
  for (i = 0; i < np; i++) {
    const uint32_t lo = p[i].vaddr > vaddr ? p[i].vaddr : vaddr;
    const uint32_t pend = p[i].vaddr + p[i].size, end = vaddr + n;
    const uint32_t hi = pend < end ? pend : end;
    if (lo >= hi) continue;
    memcpy(buf + (lo - vaddr), (const uint8_t *)p[i].data + (lo - p[i].vaddr), hi - lo);
    *done += hi - lo;
  }
}

long img_export(img_read_fn rd, img_write_fn wr, void *ctx, const ImgPatch *patches, int npatches,
                const char *name, char *err, int errlen) {
  uint8_t buf[CHUNK];
  Sha256 sha;
  uint8_t sum = 0xEF;
  uint32_t in = HDR_LEN, out = 0, patched = 0, want = 0, nseg, s;
  int hashed, i;
  sha_init(&sha);
  for (i = 0; i < npatches; i++) want += patches[i].size;
  if (rd(ctx, 0, buf, HDR_LEN)) return fail(err, errlen, "cannot read the firmware image");
  if (buf[0] != IMAGE_MAGIC) return fail(err, errlen, "the firmware image has a bad magic byte");
  nseg = buf[1];
  hashed = buf[23] == 1;
  if (!nseg || nseg > MAX_SEGMENTS) return fail(err, errlen, "the firmware image has a bad segment count");
  sha_update(&sha, buf, HDR_LEN);
  if (wr(ctx, buf, HDR_LEN)) goto wfail;
  out += HDR_LEN;
  for (s = 0; s < nseg; s++) {
    uint32_t addr, len, pos;
    if (rd(ctx, in, buf, 8)) return fail(err, errlen, "cannot read the firmware image");
    addr = rd32(buf);
    len = rd32(buf + 4);
    if (len > 16u * 1024 * 1024) return fail(err, errlen, "the firmware image has a bad segment length");
    sha_update(&sha, buf, 8);
    if (wr(ctx, buf, 8)) goto wfail;
    in += 8;
    out += 8;
    for (pos = 0; pos < len;) {
      const uint32_t n = len - pos < CHUNK ? len - pos : CHUNK;
      uint32_t j;
      if (rd(ctx, in, buf, n)) return fail(err, errlen, "cannot read the firmware image");
      apply(buf, addr + pos, n, patches, npatches, &patched);
      if (name && !pos && n >= APP_DESC_NAME + APP_DESC_NAME_LEN && rd32(buf) == APP_DESC_MAGIC) {
        memset(buf + APP_DESC_NAME, 0, APP_DESC_NAME_LEN);
        memcpy(buf + APP_DESC_NAME, name, strlen(name) < APP_DESC_NAME_LEN ? strlen(name) : APP_DESC_NAME_LEN - 1);
      }
      for (j = 0; j < n; j++) sum ^= buf[j];
      sha_update(&sha, buf, n);
      if (wr(ctx, buf, n)) goto wfail;
      in += n;
      out += n;
      pos += n;
    }
  }
  if (patched != want) return fail(err, errlen, "the export slot is not inside the firmware image");
  {
    const uint32_t pad = 15 - out % 16;
    memset(buf, 0, pad);
    buf[pad] = sum;
    sha_update(&sha, buf, pad + 1);
    if (wr(ctx, buf, pad + 1)) goto wfail;
    out += pad + 1;
  }
  if (hashed) {
    sha_final(&sha, buf);
    if (wr(ctx, buf, 32)) goto wfail;
    out += 32;
  }
  return (long)out;
wfail:
  return fail(err, errlen, "write error");
}

long img_verify(img_read_fn rd, void *ctx, uint32_t size, char *err, int errlen) {
  uint8_t buf[CHUNK], digest[32];
  Sha256 sha;
  uint8_t sum = 0xEF;
  uint32_t off = HDR_LEN, nseg, s;
  int hashed;
  sha_init(&sha);
  if (size < HDR_LEN || rd(ctx, 0, buf, HDR_LEN)) return fail(err, errlen, "image too short");
  if (buf[0] != IMAGE_MAGIC) return fail(err, errlen, "bad magic byte");
  nseg = buf[1];
  hashed = buf[23] == 1;
  if (!nseg || nseg > MAX_SEGMENTS) return fail(err, errlen, "bad segment count");
  sha_update(&sha, buf, HDR_LEN);
  for (s = 0; s < nseg; s++) {
    uint32_t len, pos;
    if (off + 8 > size || rd(ctx, off, buf, 8)) return fail(err, errlen, "truncated segment header");
    len = rd32(buf + 4);
    sha_update(&sha, buf, 8);
    off += 8;
    if (len > size - off) return fail(err, errlen, "segment runs past the end");
    for (pos = 0; pos < len;) {
      const uint32_t n = len - pos < CHUNK ? len - pos : CHUNK;
      uint32_t j;
      if (rd(ctx, off, buf, n)) return fail(err, errlen, "read error");
      for (j = 0; j < n; j++) sum ^= buf[j];
      sha_update(&sha, buf, n);
      off += n;
      pos += n;
    }
  }
  {
    const uint32_t pad = 15 - off % 16;
    if (off + pad + 1 > size || rd(ctx, off, buf, pad + 1)) return fail(err, errlen, "truncated checksum");
    if (buf[pad] != sum) return fail(err, errlen, "checksum mismatch");
    sha_update(&sha, buf, pad + 1);
    off += pad + 1;
  }
  if (hashed) {
    sha_final(&sha, digest);
    if (off + 32 > size || rd(ctx, off, buf, 32)) return fail(err, errlen, "truncated SHA-256");
    if (memcmp(buf, digest, 32)) return fail(err, errlen, "SHA-256 mismatch");
    off += 32;
  }
  return (long)off;
}
