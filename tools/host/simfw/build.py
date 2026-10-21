#!/usr/bin/env python3
"""
simfw build helper: the guest runtime that lets RV32 programs run under rvsim.

simfw = crt0 (_start), the newlib system layer (syscalls.c: _write_r, _open_r, _sbrk_r, ...,
__getreent) and opendir/readdir (dirent.c), linked with simfw.ld. main() is not part of it.
Programs are linked against the ESP toolchain's own newlib (libc.a, libm.a) and libgcc for
rv32imafc_zicsr_zifencei / ilp32f, i.e. the same library code that runs on the ESP32-P4.

  build.py build                          compile simfw into build/host/simfw/
  build.py link -o out.elf a.o b.c ...    compile .c inputs (-O1 -std=gnu11), link with simfw
        [-I dir] [-D name[=val]] [--cflags "..."]

As a module: build_simfw(), compile_c(src, obj, extra), link(inputs, out, extra_cflags).
"""
import argparse
import glob
import os
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(ROOT, "build", "host", "simfw")
LDSCRIPT = os.path.join(HERE, "simfw.ld")
SOURCES = ["crt0.S", "syscalls.c", "dirent.c"]
DEPS = SOURCES + ["sim_syscalls.h"]

RV_GCC_GLOB = os.path.expanduser(
    r"~/.emu6502-p4/data/packages/esp32/tools/esp-rv32/*/bin/riscv32-esp-elf-gcc.exe")
ARCH = ["-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f"]
USER_CFLAGS = ["-O1", "-std=gnu11"]
SIMFW_CFLAGS = ["-O2", "-std=gnu11", "-g", "-Wall", "-Wextra", "-ffunction-sections"]
LIBS = ["-Wl,--start-group", "-lc", "-lm", "-lgcc", "-Wl,--end-group"]


def find_rv_gcc():
    """Path of riscv32-esp-elf-gcc (newest Arduino ESP32 toolchain, or RV_GCC env override)."""
    if os.environ.get("RV_GCC"):
        return os.environ["RV_GCC"]
    hits = sorted(glob.glob(RV_GCC_GLOB))
    if not hits:
        sys.exit("riscv32-esp-elf-gcc not found under the Arduino ESP32 tools (set RV_GCC)")
    return hits[-1]


def _run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("command failed: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
    return r


def simfw_objects():
    return [os.path.join(OUT, os.path.splitext(s)[0] + ".o") for s in SOURCES]


def build_simfw(force=False, verbose=False):
    """Compile simfw (only what is out of date). Returns the object list in link order."""
    gcc = find_rv_gcc()
    os.makedirs(OUT, exist_ok=True)
    newest = max(os.path.getmtime(os.path.join(HERE, d)) for d in DEPS + ["build.py"])
    for src, obj in zip(SOURCES, simfw_objects()):
        if not force and os.path.exists(obj) and os.path.getmtime(obj) >= newest:
            continue
        if verbose:
            print("  cc  " + src)
        _run([gcc, *ARCH, *SIMFW_CFLAGS, "-c", os.path.join(HERE, src), "-o", obj])
    return simfw_objects()


def compile_c(src, obj, extra=()):
    """Compile one C source for the target with the user flags (-O1 -std=gnu11)."""
    _run([find_rv_gcc(), *ARCH, *USER_CFLAGS, *extra, "-c", src, "-o", obj])
    return obj


def link(inputs, out, extra_cflags=(), objdir=None):
    """Link objects (.o) and C sources (.c, compiled first) with simfw into an ELF executable."""
    objs = build_simfw()
    objdir = objdir or os.path.dirname(os.path.abspath(out))
    os.makedirs(objdir, exist_ok=True)
    user = []
    for f in inputs:
        if f.endswith(".c"):
            o = os.path.join(objdir, os.path.splitext(os.path.basename(f))[0] + ".o")
            user.append(compile_c(f, o, extra_cflags))
        else:
            user.append(f)
    _run([find_rv_gcc(), *ARCH, "-nostartfiles", "-nostdlib", "-T", LDSCRIPT, "-Wl,--gc-sections",
          *objs, *user, *LIBS, "-o", out])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="compile simfw")
    b.add_argument("--force", action="store_true")
    l = sub.add_parser("link", help="link objects / C sources with simfw")
    l.add_argument("-o", dest="out", required=True)
    l.add_argument("-I", dest="incs", action="append", default=[])
    l.add_argument("-D", dest="defs", action="append", default=[])
    l.add_argument("--cflags", default="", help="extra flags for compiling .c inputs")
    l.add_argument("inputs", nargs="+")
    a = ap.parse_args()
    try:
        if a.cmd == "build":
            for o in build_simfw(a.force, verbose=True):
                print("  " + os.path.relpath(o, ROOT))
        else:
            extra = ["-I" + i for i in a.incs] + ["-D" + d for d in a.defs] + shlex.split(a.cflags)
            link(a.inputs, a.out, extra)
    except RuntimeError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
