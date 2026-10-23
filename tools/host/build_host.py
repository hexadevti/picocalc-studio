#!/usr/bin/env python3
"""
Build the host-side RV32 test tools in one go:

  build/host/rvsim.exe    RV32IMAFC emulator (tools/host/rvsim/rvsim.c, host gcc)
  build/host/simfw/*.o    guest runtime for it (tools/host/simfw, riscv32-esp-elf-gcc)
  build/host/pcxrun.elf   .pcx module runner for rvsim (simfw/pcxrun.c + rt/pcx_load.c + rt/pcx_dbg.c):
                          rvsim --sd DIR build/host/pcxrun.elf /sd/prog.pcx [args...]
  build/host/pcc.exe      the PicoCalc C compiler (src/cc + pcc_main.c, host gcc):
                          pcc [-I dir] file.c... -o prog.pcx
  build/host/pcexport.exe the firmware exporter core (src/export/image.c + pcexport_main.c):
                          pcexport <firmware.bin> --slot 0x... --pcx prog.pcx -o app.bin

  python tools/host/build_host.py [--force]

Also importable: ensure_built() rebuilds what is out of date and returns the rvsim path.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(HERE, "simfw"))
import build as simfw  # noqa: E402  (tools/host/simfw/build.py)

RVSIM_SRC = os.path.join(HERE, "rvsim", "rvsim.c")
RVSIM_EXE = os.path.join(ROOT, "build", "host", "rvsim.exe" if os.name == "nt" else "rvsim")
HOST_CC = os.environ.get("HOST_CC", "gcc")
# -frounding-math / -ffp-contract=off: the emulator's FP code must not be folded or fused
RVSIM_FLAGS = ["-O2", "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter",
               "-frounding-math", "-ffp-contract=off", "-fno-math-errno", "-fno-strict-aliasing"]


def build_rvsim(force=False, verbose=False):
    os.makedirs(os.path.dirname(RVSIM_EXE), exist_ok=True)
    if (not force and os.path.exists(RVSIM_EXE)
            and os.path.getmtime(RVSIM_EXE) >= os.path.getmtime(RVSIM_SRC)):
        return RVSIM_EXE
    if verbose:
        print("  cc  rvsim.c")
    r = subprocess.run([HOST_CC, *RVSIM_FLAGS, RVSIM_SRC, "-o", RVSIM_EXE, "-lm"],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("rvsim build failed\n" + r.stdout + r.stderr)
    if r.stderr.strip() and verbose:
        print(r.stderr)
    return RVSIM_EXE


RT_DIR = os.path.join(ROOT, "firmware", "picocalc_studio", "src", "rt")
GEN_EXPORTS = os.path.join(ROOT, "tools", "gen_exports.py")
PCXRUN_ELF = os.path.join(ROOT, "build", "host", "pcxrun-soft.elf" if simfw.SOFT else "pcxrun.elf")
PCXRUN_DEPS = [os.path.join(HERE, "simfw", "pcxrun.c"), os.path.join(RT_DIR, "pcx_load.c"),
               os.path.join(RT_DIR, "pcx_dbg.c"),
               os.path.join(RT_DIR, "pcx.h"), os.path.join(RT_DIR, "exports.txt"), GEN_EXPORTS]


def build_pcxrun(force=False, verbose=False):
    """build/host/pcxrun.elf: pcxrun.c + pcx_load.c + pcx_dbg.c + the exports.txt table, linked with simfw."""
    objs = simfw.build_simfw(force, verbose)
    deps = PCXRUN_DEPS + [os.path.join(HERE, "simfw", d) for d in simfw.DEPS] + objs
    if (not force and os.path.exists(PCXRUN_ELF)
            and os.path.getmtime(PCXRUN_ELF) >= max(os.path.getmtime(d) for d in deps)):
        return PCXRUN_ELF
    if verbose:
        print("  ld  pcxrun.elf")
    objdir = os.path.join(ROOT, "build", "host", "pcxrun-soft" if simfw.SOFT else "pcxrun")
    os.makedirs(objdir, exist_ok=True)
    table = os.path.join(objdir, "pcxrun_exports.S")
    r = subprocess.run([sys.executable, GEN_EXPORTS, "--sim-out", table], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("gen_exports.py --sim-out failed\n" + r.stdout + r.stderr)
    table_o = os.path.join(objdir, "pcxrun_exports.o")
    simfw._run([simfw.find_rv_gcc(), *simfw.ARCH, "-c", table, "-o", table_o])
    simfw.link([os.path.join(HERE, "simfw", "pcxrun.c"), os.path.join(RT_DIR, "pcx_load.c"),
                os.path.join(RT_DIR, "pcx_dbg.c"), table_o],
               PCXRUN_ELF, ["-I", RT_DIR, "-Wall", "-Wextra", "-Werror"], objdir=objdir)
    return PCXRUN_ELF


CC_DIR = os.path.join(ROOT, "firmware", "picocalc_studio", "src", "cc")
PCC_EXE = os.path.join(ROOT, "build", "host", "pcc.exe" if os.name == "nt" else "pcc")


def build_pcc(force=False, verbose=False):
    srcs = sorted(os.path.join(CC_DIR, f) for f in os.listdir(CC_DIR) if f.endswith(".c"))
    srcs.append(os.path.join(HERE, "pcc_main.c"))
    deps = srcs + [os.path.join(CC_DIR, f) for f in os.listdir(CC_DIR) if f.endswith(".h")]
    deps.append(os.path.join(RT_DIR, "pcx.h"))
    if (not force and os.path.exists(PCC_EXE)
            and os.path.getmtime(PCC_EXE) >= max(os.path.getmtime(d) for d in deps)):
        return PCC_EXE
    if verbose:
        print("  cc  pcc")
    r = subprocess.run([HOST_CC, "-O1", "-g", "-std=gnu11", "-Wall", "-Wno-switch", "-I", CC_DIR,
                        "-I", RT_DIR, *srcs, "-o", PCC_EXE], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("pcc build failed\n" + r.stdout + r.stderr)
    return PCC_EXE


EXPORT_DIR = os.path.join(ROOT, "firmware", "picocalc_studio", "src", "export")
PCEXPORT_EXE = os.path.join(ROOT, "build", "host", "pcexport.exe" if os.name == "nt" else "pcexport")


def build_pcexport(force=False, verbose=False):
    srcs = [os.path.join(EXPORT_DIR, "image.c"), os.path.join(HERE, "pcexport_main.c")]
    deps = srcs + [os.path.join(EXPORT_DIR, "image.h")]
    if (not force and os.path.exists(PCEXPORT_EXE)
            and os.path.getmtime(PCEXPORT_EXE) >= max(os.path.getmtime(d) for d in deps)):
        return PCEXPORT_EXE
    if verbose:
        print("  cc  pcexport")
    r = subprocess.run([HOST_CC, "-O2", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", EXPORT_DIR,
                        *srcs, "-o", PCEXPORT_EXE], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("pcexport build failed\n" + r.stdout + r.stderr)
    return PCEXPORT_EXE


def ensure_built(force=False, verbose=False):
    exe = build_rvsim(force, verbose)
    simfw.build_simfw(force, verbose)
    build_pcxrun(force, verbose)
    build_pcc(force, verbose)
    build_pcexport(force, verbose)
    return exe


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="rebuild everything")
    a = ap.parse_args()
    try:
        exe = ensure_built(a.force, verbose=True)
    except RuntimeError as e:
        sys.exit(str(e))
    print("  " + os.path.relpath(exe, ROOT))
    print("  " + os.path.relpath(simfw.OUT, ROOT) + os.sep)
    print("  " + os.path.relpath(PCXRUN_ELF, ROOT))
    print("  " + os.path.relpath(PCC_EXE, ROOT))
    print("  " + os.path.relpath(PCEXPORT_EXE, ROOT))


if __name__ == "__main__":
    main()
