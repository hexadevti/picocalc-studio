#!/usr/bin/env python3
"""ESP32-P4 app image tool: the PC twin of the on-device exporter (firmware src/export).

Patches bytes at a virtual address inside an app image (e.g. the PC_SLOT export slot), then
recomputes the XOR checksum and the appended SHA-256 so the bootloader / PicoMultiLoader
accept it.

  esp_image.py info  <image.bin>
  esp_image.py selftest <image.bin>                      parse + rebuild must be byte-identical
  esp_image.py probe-slot <image.bin> --elf <app.elf> -o <out.bin>
        write the Phase-0 probe (header "PCSL" + probe_v1) into pc_slot
  esp_image.py patch <image.bin> --vaddr 0x... --data <blob> -o <out.bin>
  esp_image.py export <image.bin> --elf <app.elf> --pcx <prog.pcx> --name <n> -o <out.bin>
        what the IDE's F5 does: the .pcx in the pc_slot export slot, project_name set
  esp_image.py stamp <image.bin> [--name <n>] [--version <v>] [-o <out.bin>]
        set the app descriptor's project_name / version (PicoMultiLoader's details screen)
"""
import argparse
import hashlib
import struct
import sys

IMAGE_MAGIC = 0xE9
CHECKSUM_SEED = 0xEF
HDR_LEN = 24          # esp_image_header_t (8) + extended header (16)
SEG_HDR_LEN = 8       # esp_image_segment_header_t

SLOT_MAGIC = 0x4C534350   # "PCSL", must match spike_p0.ino / src/rt
SLOT_VERSION = 2          # src/export/image.h: SlotHeader, 64 bytes, then the .pcx
SLOT_SIZE = 256 * 1024
SLOT_AUTORUN = 1
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_VERSION = 16     # offsetof(esp_app_desc_t, version), 32 bytes
APP_DESC_NAME = 48        # offsetof(esp_app_desc_t, project_name), 32 bytes
PROBE_V1 = [
    0xff010113, 0x00112623, 0x00812423, 0x00060413, 0x000580e7,
    0x00850533, 0x00150513, 0x00000297, 0x01c2a303, 0x00650533,
    0x00812403, 0x00c12083, 0x01010113, 0x00008067, 0x000003e8,
]


class Image:
    def __init__(self, data: bytes):
        if len(data) < HDR_LEN or data[0] != IMAGE_MAGIC:
            raise ValueError("not an ESP app image (bad magic)")
        self.header = bytearray(data[:HDR_LEN])
        nseg = data[1]
        self.chip_id = struct.unpack_from("<H", data, 12)[0]
        self.hash_appended = data[23] == 1
        self.segments = []   # [load_addr, bytearray, file_offset_of_data]
        off = HDR_LEN
        for _ in range(nseg):
            addr, length = struct.unpack_from("<II", data, off)
            off += SEG_HDR_LEN
            if off + length > len(data):
                raise ValueError("segment runs past end of file")
            self.segments.append([addr, bytearray(data[off:off + length]), off])
            off += length
        self.data_end = off
        pad = 15 - (off % 16)
        self.checksum = data[off + pad]
        self.body_len = off + pad + 1
        self.trailer = data[self.body_len + (32 if self.hash_appended else 0):]   # anything after (rare)
        self.stored_hash = data[self.body_len:self.body_len + 32] if self.hash_appended else None

    def compute_checksum(self) -> int:
        c = CHECKSUM_SEED
        for _, seg, _ in self.segments:
            for b in seg:
                c ^= b
        return c

    def build(self) -> bytes:
        out = bytearray(self.header)
        for addr, seg, _ in self.segments:
            out += struct.pack("<II", addr, len(seg)) + seg
        out += bytes(15 - (len(out) % 16))
        out.append(self.compute_checksum())
        if self.hash_appended:
            out += hashlib.sha256(out).digest()
        return bytes(out + self.trailer)

    def find(self, vaddr: int, length: int):
        for s in self.segments:
            addr, seg, _ = s
            if addr <= vaddr and vaddr + length <= addr + len(seg):
                return s, vaddr - addr
        raise ValueError(f"0x{vaddr:08x}+{length} is not inside any segment")

    def patch(self, vaddr: int, blob: bytes):
        (addr, seg, foff), off = self.find(vaddr, len(blob))
        seg[off:off + len(blob)] = blob
        return foff + off

    def read(self, vaddr: int, length: int) -> bytes:
        (_, seg, _), off = self.find(vaddr, length)
        return bytes(seg[off:off + length])


def elf_symbol(path: str, name: str) -> int:
    """Minimal ELF32 little-endian symtab lookup (no external deps)."""
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1:
        raise ValueError("not an ELF32 file")
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    for s in secs:
        if s[1] != 2:          # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for i in range(s[5] // 16):
            st_name, st_value = struct.unpack_from("<II", d, s[4] + i * 16)
            end = d.index(b"\0", strtab[4] + st_name)
            if d[strtab[4] + st_name:end].decode() == name:
                return st_value
    raise KeyError(f"symbol {name} not found in {path}")


def cmd_info(a):
    img = Image(open(a.image, "rb").read())
    print(f"chip id {img.chip_id}, {len(img.segments)} segments, hash appended: {img.hash_appended}")
    for i, (addr, seg, foff) in enumerate(img.segments):
        print(f"  {i}: 0x{addr:08x} len 0x{len(seg):06x} file 0x{foff:06x}")
    ok_c = img.compute_checksum() == img.checksum
    ok_h = (not img.hash_appended) or hashlib.sha256(img.build()[:img.body_len]).digest() == img.stored_hash
    print(f"checksum 0x{img.checksum:02x} {'valid' if ok_c else 'INVALID'}, sha256 {'valid' if ok_h else 'INVALID'}")
    for _, seg, _ in img.segments:
        if struct.unpack_from("<I", seg)[0] == APP_DESC_MAGIC:
            field = lambda o: bytes(seg[o:o + 32]).split(b"\0")[0].decode(errors="replace")
            print(f"app descriptor: name {field(APP_DESC_NAME)!r}, version {field(APP_DESC_VERSION)!r}")
    return 0 if ok_c and ok_h else 1


def cmd_selftest(a):
    raw = open(a.image, "rb").read()
    same = Image(raw).build() == raw
    print("rebuild identical" if same else "REBUILD DIFFERS")
    return 0 if same else 1


def write_patched(img, vaddr, blob, out):
    foff = img.patch(vaddr, blob)
    open(out, "wb").write(img.build())
    print(f"patched {len(blob)} bytes at vaddr 0x{vaddr:08x} (file offset 0x{foff:06x}) -> {out}")


def cmd_probe_slot(a):
    img = Image(open(a.image, "rb").read())
    vaddr = elf_symbol(a.elf, a.symbol)
    if img.read(vaddr, 16) != b"\xff" * 16:
        print("slot is not empty (already patched?)")
        return 1
    hdr = struct.pack("<IIII", SLOT_MAGIC, 16, 1, 0)
    write_patched(img, vaddr, hdr + struct.pack(f"<{len(PROBE_V1)}I", *PROBE_V1), a.out)
    return 0


def cmd_patch(a):
    img = Image(open(a.image, "rb").read())
    write_patched(img, int(a.vaddr, 0), open(a.data, "rb").read(), a.out)
    return 0


def cmd_stamp(a):
    img = Image(open(a.image, "rb").read())
    for _, seg, _ in img.segments:
        if struct.unpack_from("<I", seg)[0] == APP_DESC_MAGIC:
            if a.version:
                seg[APP_DESC_VERSION:APP_DESC_VERSION + 32] = a.version.encode()[:31].ljust(32, b"\0")
            if a.name:
                seg[APP_DESC_NAME:APP_DESC_NAME + 32] = a.name.encode()[:31].ljust(32, b"\0")
            open(a.out or a.image, "wb").write(img.build())
            print(f"app descriptor: name {a.name!r}, version {a.version!r}")
            return 0
    print("no app descriptor found")
    return 1


def cmd_export(a):
    img = Image(open(a.image, "rb").read())
    pcx = open(a.pcx, "rb").read()
    if len(pcx) > SLOT_SIZE - 64:
        print("program too big for the slot")
        return 1
    name = a.name.encode()[:43]
    slot = struct.pack("<IIIII", SLOT_MAGIC, SLOT_VERSION, SLOT_AUTORUN, 64, len(pcx)) + name.ljust(44, b"\0") + pcx
    slot += b"\xff" * (SLOT_SIZE - len(slot))
    for _, seg, _ in img.segments:
        if struct.unpack_from("<I", seg)[0] == APP_DESC_MAGIC:
            seg[APP_DESC_NAME:APP_DESC_NAME + 32] = a.name.encode()[:31].ljust(32, b"\0")
    write_patched(img, elf_symbol(a.elf, "pc_slot"), slot, a.out)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("info"); s.add_argument("image"); s.set_defaults(fn=cmd_info)
    s = sub.add_parser("selftest"); s.add_argument("image"); s.set_defaults(fn=cmd_selftest)
    s = sub.add_parser("probe-slot"); s.add_argument("image"); s.add_argument("--elf", required=True)
    s.add_argument("--symbol", default="pc_slot"); s.add_argument("-o", "--out", required=True)
    s.set_defaults(fn=cmd_probe_slot)
    s = sub.add_parser("patch"); s.add_argument("image"); s.add_argument("--vaddr", required=True)
    s.add_argument("--data", required=True); s.add_argument("-o", "--out", required=True)
    s.set_defaults(fn=cmd_patch)
    s = sub.add_parser("export"); s.add_argument("image"); s.add_argument("--elf", required=True)
    s.add_argument("--pcx", required=True); s.add_argument("--name", required=True)
    s.add_argument("-o", "--out", required=True); s.set_defaults(fn=cmd_export)
    s = sub.add_parser("stamp"); s.add_argument("image"); s.add_argument("--name"); s.add_argument("--version")
    s.add_argument("-o", "--out"); s.set_defaults(fn=cmd_stamp)
    a = p.parse_args()
    sys.exit(a.fn(a))


if __name__ == "__main__":
    main()
