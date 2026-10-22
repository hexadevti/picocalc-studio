#!/usr/bin/env python3
"""
Compiler memory: the arena peak (pcc --mem) for every project in tests/projects.

The device is 32-bit, so this builds its own 32-bit pcc (build/host/pcc32.exe) to get the same
struct sizes; a 64-bit pcc overstates the arena by about 1.6x. The 32-bit compiler is found from
$PCC32_CC, then C:/msys64/mingw32/bin/gcc.exe, then i686-w64-mingw32-gcc, then `gcc -m32`.

  python tools/host/mem.py                    print the table
  python tools/host/mem.py --save NAME        also store it as tests/bench/mem_NAME.json
  python tools/host/mem.py --compare NAME     add a column with the change against mem_NAME.json

Target for the RP2350 (Pico 2 W, 520 KB SRAM, no PSRAM): about 200 KB peak.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_host  # noqa: E402

ROOT = build_host.ROOT
PROJECTS = os.path.join(ROOT, "tests", "projects")
BENCH_DIR = os.path.join(ROOT, "tests", "bench")
PCC32_EXE = os.path.join(ROOT, "build", "host", "pcc32.exe" if os.name == "nt" else "pcc32")
MEM_RE = re.compile(r"mem: arena peak (\d+) image (\d+)")


def find_cc32():
    """[compiler, flags...] that produces 32-bit x86 code."""
    if os.environ.get("PCC32_CC"):
        return os.environ["PCC32_CC"].split()
    msys = "C:/msys64/mingw32/bin/gcc.exe"
    if os.path.exists(msys):
        return [msys]
    if shutil.which("i686-w64-mingw32-gcc"):
        return ["i686-w64-mingw32-gcc"]
    return ["gcc", "-m32"]


def build_pcc32():
    cc_dir = build_host.CC_DIR
    srcs = sorted(os.path.join(cc_dir, f) for f in os.listdir(cc_dir) if f.endswith(".c"))
    srcs.append(os.path.join(build_host.HERE, "pcc_main.c"))
    cc = find_cc32()
    env = dict(os.environ)
    # msys2's mingw32 gcc needs its own bin dir on PATH for cc1/as and its DLLs.
    env["PATH"] = os.path.dirname(cc[0]) + os.pathsep + env.get("PATH", "")
    # -mno-ms-bitfields: MinGW's default MSVC layout does not pack Token's bitfields like RV32 gcc.
    flags = ["-mno-ms-bitfields"] if "mingw" in " ".join(cc) or cc[0].endswith(".exe") else []
    r = subprocess.run([*cc, *flags, "-O1", "-std=gnu11", "-w", "-I", cc_dir, "-I", build_host.RT_DIR,
                        *srcs, "-o", PCC32_EXE], capture_output=True, text=True, env=env)
    if r.returncode:
        sys.exit(f"32-bit pcc build failed ({' '.join(cc)})\n{r.stdout}{r.stderr}")
    return env


def measure(env):
    """{project: (peak, image)}"""
    res = {}
    with tempfile.TemporaryDirectory() as tmp:
        for name in sorted(os.listdir(PROJECTS)):
            d = os.path.join(PROJECTS, name)
            srcs = sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".c"))
            if not srcs:
                continue
            p = subprocess.run([PCC32_EXE, "--mem", *srcs, "-o", os.path.join(tmp, name + ".pcx")],
                               capture_output=True, text=True, env=env)
            m = MEM_RE.search(p.stderr)
            if p.returncode or not m:
                sys.exit(f"{name}: pcc32 failed ({p.returncode})\n{p.stdout}{p.stderr}")
            res[name] = (int(m.group(1)), int(m.group(2)))
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--save", metavar="NAME")
    ap.add_argument("--compare", metavar="NAME")
    args = ap.parse_args()

    env = build_pcc32()
    res = measure(env)
    ref = None
    if args.compare:
        with open(os.path.join(BENCH_DIR, f"mem_{args.compare}.json")) as f:
            ref = json.load(f)

    print(f"{'project':<15}{'peak KB':>9}{'image':>8}" + (f"{args.compare + ' KB':>14}{'change':>8}" if ref else ""))
    for name, (peak, image) in res.items():
        line = f"{name:<15}{peak / 1024:>9.0f}{image:>8}"
        if ref and name in ref:
            old = ref[name]["peak"]
            line += f"{old / 1024:>14.0f}{(peak - old) * 100 / old:>+7.0f}%"
        print(line)
    print(f"{'max':<15}{max(p for p, _ in res.values()) / 1024:>9.0f}")

    if args.save:
        with open(os.path.join(BENCH_DIR, f"mem_{args.save}.json"), "w") as f:
            json.dump({n: {"peak": p, "image": i} for n, (p, i) in res.items()}, f, indent=1)


if __name__ == "__main__":
    main()
