#!/usr/bin/env python3
"""
Compiler speed benchmarks (tests/bench/main.c) under the RV32 emulator.

For every benchmark it reports the instructions executed (rvsim --stats, minus the startup cost
measured with `bench none`) for code built by pcc and by riscv32-esp-elf-gcc -O2 (the target to
approach), the checksum (must match gcc's), and the .pcx text size.

  python tools/host/bench.py                    run, print the table
  python tools/host/bench.py --save NAME        also store the results as tests/bench/NAME.json
  python tools/host/bench.py --compare NAME     add a column with the speedup against NAME.json

On the device: copy tests/bench/main.c into a project and run it; it prints microseconds.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_host  # noqa: E402
from simfw import build as simfw  # noqa: E402

ROOT = build_host.ROOT
SRC = os.path.join(ROOT, "tests", "bench", "main.c")
BENCH_DIR = os.path.dirname(SRC)
NAMES = ["fib", "sieve", "crc", "mandel", "sort", "str", "matrix"]
STATS_RE = re.compile(rb"rvsim: (\d+) instructions")


def rvsim(elf, args, sd):
    cmd = [build_host.RVSIM_EXE, "--stats", "--sd", sd, elf, *args]
    p = subprocess.run(cmd, capture_output=True, timeout=600)
    m = STATS_RE.search(p.stderr)
    if p.returncode or not m:
        sys.exit(f"{' '.join(cmd)} failed ({p.returncode})\n{p.stdout.decode(errors='replace')}"
                 f"{p.stderr.decode(errors='replace')}")
    return int(m.group(1)), p.stdout.decode(errors="replace")


def measure(elf, prefix, sd):
    """{name: (instructions, checksum)} for one build; prefix = guest args before the bench name."""
    base, _ = rvsim(elf, prefix + ["none"], sd)
    res = {}
    for n in NAMES:
        count, out = rvsim(elf, prefix + [n], sd)
        fields = out.split()
        if len(fields) < 2 or fields[0] != n:
            sys.exit(f"{n}: unexpected output {out!r}")
        res[n] = (count - base, fields[1])
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--save", metavar="NAME")
    ap.add_argument("--compare", metavar="NAME")
    ap.add_argument("--no-gcc", action="store_true", help="skip the gcc -O2 reference")
    a = ap.parse_args()

    build_host.ensure_built()
    with tempfile.TemporaryDirectory() as tmp:
        sd = os.path.join(tmp, "sd")
        os.makedirs(sd)
        pcx = os.path.join(sd, "bench.pcx")
        r = subprocess.run([build_host.PCC_EXE, "-DBENCH_SIM", SRC, "-o", pcx], capture_output=True, text=True)
        if r.returncode:
            sys.exit("pcc failed\n" + r.stdout + r.stderr)
        with open(pcx, "rb") as f:
            text_size = struct.unpack_from("<I", f.read(12), 8)[0]
        pcc = measure(build_host.PCXRUN_ELF, ["/sd/bench.pcx"], sd)
        gcc = None
        if not a.no_gcc:
            elf = os.path.join(tmp, "bench_gcc.elf")
            simfw.link([SRC], elf, ["-O2", "-ffp-contract=off", "-DBENCH_SIM"], objdir=tmp)
            gcc = measure(elf, [], sd)

    old = None
    if a.compare:
        with open(os.path.join(BENCH_DIR, a.compare + ".json")) as f:
            old = json.load(f)

    hdr = f"{'bench':8}{'pcc insns':>14}{'gcc -O2':>14}{'pcc/gcc':>9}"
    if old:
        hdr += f"{a.compare:>14}{'speedup':>9}"
    print(hdr)
    bad = 0
    tot = [0, 0, 0]
    for n in NAMES:
        cnt, sum_ = pcc[n]
        line = f"{n:8}{cnt:>14,}"
        tot[0] += cnt
        if gcc:
            g, gsum = gcc[n]
            tot[1] += g
            line += f"{g:>14,}{cnt / g:>8.2f}x"
            if gsum != sum_:
                line += f"  CHECKSUM {sum_} != gcc {gsum}"
                bad += 1
        else:
            line += f"{'':>23}"
        if old:
            o = old["insns"][n]
            tot[2] += o
            line += f"{o:>14,}{o / cnt:>8.2f}x"
            if old["sums"][n] != sum_:
                line += f"  CHECKSUM changed (was {old['sums'][n]})"
                bad += 1
        print(line)
    line = f"{'total':8}{tot[0]:>14,}"
    line += f"{tot[1]:>14,}{tot[0] / tot[1]:>8.2f}x" if gcc else f"{'':>23}"
    if old:
        line += f"{tot[2]:>14,}{tot[2] / tot[0]:>8.2f}x"
    print(line)
    print(f".pcx text: {text_size:,} bytes" + (f" (was {old['text_size']:,})" if old else ""))

    if a.save:
        path = os.path.join(BENCH_DIR, a.save + ".json")
        with open(path, "w", newline="\n") as f:
            json.dump({"insns": {n: pcc[n][0] for n in NAMES}, "sums": {n: pcc[n][1] for n in NAMES},
                       "text_size": text_size}, f, indent=1)
            f.write("\n")
        print("saved " + os.path.relpath(path, ROOT))
    sys.exit(bad)


if __name__ == "__main__":
    main()
