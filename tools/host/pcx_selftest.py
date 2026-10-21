#!/usr/bin/env python3
"""
Self-test for the .pcx loader (firmware/picocalc_studio/src/rt/pcx_load.c) and the simulator
runner (build/host/pcxrun.elf), independent of the compiler.

Assembles a small main() with riscv32-esp-elf-as, packs it into a .pcx by hand and runs it with
  rvsim --sd <tmp> build/host/pcxrun.elf /sd/<name>.pcx
Covers PAIR relocations (auipc + addi / lw / jalr / sw) to ram, bss and an import, ABS32
relocations in data to ram and to text, plus load errors (undefined symbol, truncated file,
bad magic). The line table is checked with a host-gcc build of pcx_parse + pcx_lookup_line.

  python tools/host/pcx_selftest.py          exit status = number of failures
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_host  # noqa: E402
from simfw import build as simfw  # noqa: E402

RT_DIR = build_host.RT_DIR
PCX_MAGIC, PCX_VERSION, NO_ENTRY = 0x31584350, 1, 0xFFFFFFFF
W_RAM = 0x80000000
R_PAIR, R_ABS32 = 0, 1
T_TEXT, T_RAM, T_IMPORT = 0, 1, 2

ASM = r"""
    .option norvc
    .option norelax
    .text
    .globl main
main:
    addi sp, sp, -16
    sw   ra, 12(sp)
    sw   s0, 8(sp)
r_str:                      # PAIR addi -> ram+0 ("hello pcx")
    auipc a0, 0
    addi  a0, a0, 0
r_puts1:                    # PAIR jalr -> import puts
    auipc t1, 0
    jalr  ra, 0(t1)
r_ptr:                      # PAIR lw -> ram+24 (ABS32 pointer to ram+12, "abs32 ok")
    auipc a0, 0
    lw    a0, 0(a0)
r_puts2:
    auipc t1, 0
    jalr  ra, 0(t1)
r_fptr:                     # PAIR lw -> ram+28 (ABS32 pointer to text: helper)
    auipc t0, 0
    lw    t0, 0(t0)
    jalr  ra, 0(t0)
    mv    s0, a0            # 7
    li    t2, 42
r_bss_st:                   # PAIR sw -> bss (ram+48)
    auipc t0, 0
    sw    t2, 0(t0)
r_bss_ld:                   # PAIR lw -> bss (ram+48)
    auipc t1, 0
    lw    t1, 0(t1)
    add   a0, s0, t1
    addi  a0, a0, -49       # 0 when helper() == 7 and the bss round trip worked
r_far0:                     # PAIR addi -> ram+0x400; must equal the ABS32 word at ram+36
    auipc t1, 0
    addi  t1, t1, 0
r_w0:
    auipc t2, 0
    lw    t2, 0(t2)
    sub   t1, t1, t2
    or    a0, a0, t1
r_far1:                     # ram+0xc00 (one of the two has a negative low part)
    auipc t1, 0
    addi  t1, t1, 0
r_w1:
    auipc t2, 0
    lw    t2, 0(t2)
    sub   t1, t1, t2
    or    a0, a0, t1
r_far2:                     # ram-0x12345
    auipc t1, 0
    addi  t1, t1, 0
r_w2:
    auipc t2, 0
    lw    t2, 0(t2)
    sub   t1, t1, t2
    or    a0, a0, t1
    lw    s0, 8(sp)
    lw    ra, 12(sp)
    addi  sp, sp, 16
    ret
helper:
    li    a0, 7
    ret
"""

DATA = (b"hello pcx\0\0\0"      # 0
        b"abs32 ok\0\0\0\0"     # 12
        + struct.pack("<II", 0, 0)  # 24: -> ram+12, 28: -> text helper (relocated)
        + struct.pack("<I", 0x12345678)   # 32: plain word
        + struct.pack("<III", 0, 0, 0))   # 36, 40, 44: ABS32 ram+0x400, ram+0xc00, ram-0x12345
DATA_SIZE = len(DATA)               # 48
BSS_SIZE = 8                        # 48..55

failures = 0


def check(cond, msg):
    global failures
    if cond:
        print("  ok   " + msg)
    else:
        failures += 1
        print("  FAIL " + msg)


def tool(name):
    return simfw.find_rv_gcc().replace("riscv32-esp-elf-gcc", "riscv32-esp-elf-" + name)


def assemble(tmp):
    s, o, b = (os.path.join(tmp, "m." + e) for e in ("s", "o", "bin"))
    with open(s, "w") as f:
        f.write(ASM)
    simfw._run([tool("as"), "-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f", s, "-o", o])
    simfw._run([tool("objcopy"), "-O", "binary", "-j", ".text", o, b])
    syms = {}
    for line in simfw._run([tool("nm"), o]).stdout.splitlines():
        v, _, n = line.split()
        syms[n] = int(v, 16)
    with open(b, "rb") as f:
        return f.read(), syms


def pack(text, data, bss, imports, relocs, lines=(), files=(), entry_main=0, magic=PCX_MAGIC,
         ram_align=8):
    strtab = b""
    def add(s):
        nonlocal strtab
        off = len(strtab)
        strtab += s.encode() + b"\0"
        return off
    imp = [add(n) for n in imports]
    fil = [add(n) for n in files]
    assert len(text) % 4 == 0
    hdr = struct.pack("<14I", magic, PCX_VERSION, len(text), len(data), bss, ram_align, len(imp),
                      len(relocs), len(lines), len(fil), len(strtab), entry_main, NO_ENTRY, NO_ENTRY)
    out = hdr + text + data
    out += b"\0" * (-len(out) % 4)
    out += b"".join(struct.pack("<I", x) for x in imp)
    out += b"".join(struct.pack("<IIi", w, t, a) for w, t, a in relocs)
    out += b"".join(struct.pack("<IHH", o, fi, ln) for o, fi, ln in lines)
    out += b"".join(struct.pack("<I", x) for x in fil)
    return out + strtab


def run_pcx(sd, name, blob):
    with open(os.path.join(sd, name), "wb") as f:
        f.write(blob)
    return subprocess.run([build_host.RVSIM_EXE, "--sd", sd, "--max-insns", "50e6",
                           build_host.PCXRUN_ELF, "/sd/" + name], capture_output=True)


HOST_TEST = r"""
#include <stdio.h>
#include <stdlib.h>
#include "pcx.h"
int main(int argc, char **argv) {
  static unsigned buf[4096];
  FILE *f = fopen(argv[1], "rb");
  size_t n = fread(buf, 1, sizeof buf, f);
  PcxView v; char err[100]; const char *file; int line;
  if (pcx_parse(buf, n, &v, err, sizeof err)) { printf("parse: %s\n", err); return 1; }
  for (int i = 2; i < argc; i++) {
    if (pcx_lookup_line(&v, (unsigned)strtoul(argv[i], 0, 0), &file, &line)) printf("%s:%d\n", file, line);
    else printf("none\n");
  }
  return 0;
}
"""


def main():
    build_host.ensure_built()
    with tempfile.TemporaryDirectory() as tmp:
        sd = os.path.join(tmp, "sd")
        os.makedirs(sd)
        text, sym = assemble(tmp)
        relocs = [
            (R_PAIR << 28 | sym["r_str"], T_RAM << 24, 0),
            (R_PAIR << 28 | sym["r_puts1"], T_IMPORT << 24 | 0, 0),
            (R_PAIR << 28 | sym["r_ptr"], T_RAM << 24, 24),
            (R_PAIR << 28 | sym["r_puts2"], T_IMPORT << 24 | 0, 0),
            (R_PAIR << 28 | sym["r_fptr"], T_RAM << 24, 28),
            (R_PAIR << 28 | sym["r_bss_st"], T_RAM << 24, 48),
            (R_PAIR << 28 | sym["r_bss_ld"], T_RAM << 24, 48),
            (W_RAM | R_ABS32 << 28 | 24, T_RAM << 24, 12),
            (W_RAM | R_ABS32 << 28 | 28, T_TEXT << 24, sym["helper"]),
        ]
        for i, add in enumerate((0x400, 0xC00, -0x12345)):
            relocs += [(R_PAIR << 28 | sym[f"r_far{i}"], T_RAM << 24, add),
                       (R_PAIR << 28 | sym[f"r_w{i}"], T_RAM << 24, 36 + 4 * i),
                       (W_RAM | R_ABS32 << 28 | (36 + 4 * i), T_RAM << 24, add)]
        lines = [(0, 0, 10), (sym["r_str"], 0, 11), (sym["r_ptr"], 0, 12), (sym["helper"], 1, 3)]
        good = pack(text, DATA, BSS_SIZE, ["puts"], relocs, lines, ["main.c", "util.c"])

        print("[pcx selftest] rvsim + pcxrun.elf")
        p = run_pcx(sd, "good.pcx", good)
        out = p.stdout.replace(b"\r\n", b"\n")
        check(p.returncode == 0 and out == b"hello pcx\nabs32 ok\n",
              f"PAIR/ABS32 relocations, import call (status {p.returncode}, out {out!r}, err {p.stderr!r})")

        bad = pack(text, DATA, BSS_SIZE, ["puts", "gfx_clear"], relocs)
        p = run_pcx(sd, "undef.pcx", bad)
        check(p.returncode == 127 and b"pcxrun: undefined symbol 'gfx_clear'" in p.stderr,
              f"undefined symbol rejected ({p.returncode}, {p.stderr!r})")

        p = run_pcx(sd, "trunc.pcx", good[:-3])
        check(p.returncode == 127 and b"pcxrun: truncated" in p.stderr, f"truncated file rejected ({p.stderr!r})")

        p = run_pcx(sd, "magic.pcx", pack(text, DATA, BSS_SIZE, ["puts"], relocs, magic=0x1234))
        check(p.returncode == 127 and b"bad magic" in p.stderr, f"bad magic rejected ({p.stderr!r})")

        ram_pair = pack(text, DATA, BSS_SIZE, ["puts"], relocs + [(W_RAM | 0 << 28 | 0, T_RAM << 24, 0)])
        p = run_pcx(sd, "rampair.pcx", ram_pair)
        check(p.returncode == 127 and b"PAIR relocation in the ram block" in p.stderr,
              f"PAIR in ram rejected ({p.stderr!r})")

        # line table on the host (host gcc build of the loader)
        gcc = os.environ.get("HOST_CC", "gcc")
        if shutil.which(gcc):
            exe = os.path.join(tmp, "lines.exe")
            c = os.path.join(tmp, "lines.c")
            with open(c, "w") as f:
                f.write(HOST_TEST)
            r = subprocess.run([gcc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", RT_DIR, c,
                                os.path.join(RT_DIR, "pcx_load.c"), "-o", exe], capture_output=True, text=True)
            if r.returncode:
                check(False, "host build of pcx_load.c\n" + r.stderr)
            else:
                offs = [0, sym["r_str"] - 4, sym["r_str"], sym["r_ptr"] + 6, sym["helper"], len(text) - 4]
                r = subprocess.run([exe, os.path.join(sd, "good.pcx"), *map(str, offs)],
                                   capture_output=True, text=True)
                want = "main.c:10\nmain.c:10\nmain.c:11\nmain.c:12\nutil.c:3\nutil.c:3\n"
                check(r.stdout.replace("\r\n", "\n") == want, f"pcx_lookup_line on the host ({r.stdout!r})")
    print(f"\n{failures} failure(s)")
    sys.exit(failures)


if __name__ == "__main__":
    main()
