#!/usr/bin/env python3
"""
PicoCalc Studio compiler test harness (host side).

Modes
-----
  ref     Build every tests/unit/*.c with the host 32-bit gcc (flags chosen to mimic RV32 ilp32f:
          unsigned plain char, 8-byte double/long long alignment, GCC bitfield layout), run it and
          compare stdout with tests/unit/*.expected. --update rewrites the .expected files.
  target  Compile every test with riscv32-esp-elf-gcc (-march=rv32imafc -mabi=ilp32f). Proves each
          test is valid C for the real target and checks the _Static_assert layout claims.
  errors  Check that every tests/errors/*.c is rejected by gcc at the line its EXPECT header names.
          (Validates the tests themselves; the same headers are later checked against pcc.)
  simref  Build every tests/unit/*.c with riscv32-esp-elf-gcc (-O1, rv32imafc/ilp32f, the real
          ESP newlib), link it with simfw and run it under rvsim (tools/host/rvsim); stdout must
          match .expected byte for byte. TEST_DIR keeps its device default (/sd/pico-dev/tests/tmp),
          served from a temporary --sd directory. Builds rvsim/simfw first if out of date.
  pcc     Run the PicoCalc compiler on every test: --pcc "<command with {src} {out}>" compiles,
          --exec "<command with {out}>" runs the module (device bridge or RV32 emulator).
          --exec rvsim is shorthand for
            "build/host/rvsim.exe --sd <tmp>/sd build/host/pcxrun.elf {gout}"
          where the modules are written to <tmp>/sd/pcx/ and {gout} is the guest path
          (/sd/pcx/tNN.pcx); a custom --exec may use {gout} too.
          Unit tests must reproduce the .expected output; error tests must fail as declared.
  all     ref + target + errors + simref (default).

Exit status is the number of failures (0 = all good).
"""
import argparse
import glob
import os
import re
import shlex
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_host  # noqa: E402  (rvsim + simfw builder)
from simfw import build as simfw  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
UNIT = os.path.join(ROOT, "tests", "unit")
ERRS = os.path.join(ROOT, "tests", "errors")
SDK_INC = os.path.join(ROOT, "sdcard", "pico-dev", "include")

HOST_GCC_CANDIDATES = [r"C:\msys64\mingw32\bin\gcc.exe", "i686-w64-mingw32-gcc", "gcc"]
RV_GCC_GLOB = os.path.expanduser(
    r"~/.emu6502-p4/data/packages/esp32/tools/esp-rv32/*/bin/riscv32-esp-elf-gcc.exe")

HOST_FLAGS = [
    "-std=gnu11", "-O1", "-w",
    "-funsigned-char",          # RISC-V plain char is unsigned
    "-mno-ms-bitfields",        # SysV bitfield packing, not MSVC
    "-D__USE_MINGW_ANSI_STDIO=1",  # C99-conformant printf (%lld, %e exponent digits)
    "-ffloat-store",            # no x87 excess precision: match IEEE single/double rounding
    "-msse2", "-mfpmath=sse", "-static",
]
RV_FLAGS = ["-std=gnu11", "-O1", "-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f",
            "-c", "-Werror=implicit-function-declaration"]

EXPECT_RE = re.compile(r"//\s*EXPECT:\s*(\w+)\s+line=([\d|]+)\s+match=(.+)$")

failures = 0


def fail(msg):
    global failures
    failures += 1
    print("  FAIL " + msg)


def find_host_gcc():
    for c in HOST_GCC_CANDIDATES:
        if os.path.isabs(c) and os.path.exists(c):
            # MSYS2 gcc needs its own bin dir on PATH to find cc1/as and their DLLs
            os.environ["PATH"] = os.path.dirname(c) + os.pathsep + os.environ["PATH"]
        try:
            out = subprocess.run([c, "-dumpmachine"], capture_output=True, text=True)
        except OSError:
            continue
        if out.returncode == 0 and ("i686" in out.stdout or "i386" in out.stdout):
            return c
    sys.exit("no 32-bit host gcc found (install MSYS2 mingw32 gcc)")


def find_rv_gcc():
    hits = sorted(glob.glob(RV_GCC_GLOB))
    if not hits:
        sys.exit("riscv32-esp-elf-gcc not found under the Arduino ESP32 tools")
    return hits[-1]


def unit_tests():
    return sorted(glob.glob(os.path.join(UNIT, "t*.c")))


def error_tests():
    return sorted(glob.glob(os.path.join(ERRS, "err*.c")))


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def mode_ref(update):
    gcc = find_host_gcc()
    print(f"[ref] host compiler: {gcc}")
    with tempfile.TemporaryDirectory() as tmp:
        for src in unit_tests():
            name = os.path.splitext(os.path.basename(src))[0]
            exe = os.path.join(tmp, name + ".exe")
            tdir = os.path.join(tmp, "sd").replace("\\", "/")
            r = run([gcc, *HOST_FLAGS, f'-DTEST_DIR="{tdir}"', "-I", UNIT, src, "-o", exe, "-lm"])
            if r.returncode:
                fail(f"{name}: host build failed\n{r.stderr}")
                continue
            p = run([exe], cwd=tmp, timeout=60)
            out = p.stdout.replace("\r\n", "\n")
            if p.returncode != 0:
                fail(f"{name}: exit status {p.returncode}")
                continue
            exp_path = os.path.splitext(src)[0] + ".expected"
            if update or not os.path.exists(exp_path):
                with open(exp_path, "w", newline="\n") as f:
                    f.write(out)
                print(f"  wrote {os.path.basename(exp_path)} ({out.count(chr(10))} lines)")
                continue
            with open(exp_path, newline="") as f:
                want = f.read()
            if out != want:
                fail(f"{name}: output differs from .expected\n{diff(want, out)}")
            else:
                print(f"  ok   {name}")


def mode_target():
    gcc = find_rv_gcc()
    print(f"[target] {gcc}")
    with tempfile.TemporaryDirectory() as tmp:
        for src in unit_tests():
            name = os.path.basename(src)
            r = run([gcc, *RV_FLAGS, "-I", UNIT, src, "-o", os.path.join(tmp, "x.o")])
            if r.returncode:
                fail(f"{name}: riscv32 compile failed\n{r.stderr}")
            else:
                print(f"  ok   {name}")
        # Tier B projects compile against the SDK header for the real target too
        for proj in sorted(glob.glob(os.path.join(ROOT, "tests", "projects", "*", ""))):
            pname = os.path.basename(os.path.dirname(proj))
            ok = True
            for src in sorted(glob.glob(os.path.join(proj, "*.c"))):
                r = run([gcc, *RV_FLAGS, "-Wall", "-Werror", "-I", SDK_INC, "-I", proj, src,
                         "-o", os.path.join(tmp, "p.o")])
                if r.returncode:
                    ok = False
                    fail(f"project {pname}/{os.path.basename(src)}\n{r.stderr}")
            if ok:
                print(f"  ok   project {pname}")


def sim_exec(elf, args=(), sd=None, timeout=120, max_insns=None):
    """Run an RV32 ELF under rvsim. Returns the CompletedProcess with stdout/stderr as bytes.
    sd: host directory behind the guest's /sd (rvsim's default temp dir if None)."""
    cmd = [build_host.RVSIM_EXE]
    if sd:
        cmd += ["--sd", sd]
    if max_insns:
        cmd += ["--max-insns", str(max_insns)]
    try:
        return subprocess.run(cmd + [elf, *args], capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        return subprocess.CompletedProcess(cmd, -1, e.stdout or b"", (e.stderr or b"") + b"\n[timeout]")


def new_sd_dir(tmp):
    """Fresh SD-card directory, laid out like the device (/sd/pico-dev/tests exists)."""
    sd = os.path.join(tmp, "sd")
    os.makedirs(os.path.join(sd, "pico-dev", "tests"), exist_ok=True)
    return sd


def mode_simref():
    try:
        build_host.ensure_built()
    except RuntimeError as e:
        fail(f"simref: building rvsim/simfw failed\n{e}")
        return
    print(f"[simref] {os.path.relpath(build_host.RVSIM_EXE, ROOT)} + simfw, riscv32-esp-elf-gcc -O1")
    with tempfile.TemporaryDirectory() as tmp:
        sd = new_sd_dir(tmp)
        for src in unit_tests():
            name = os.path.splitext(os.path.basename(src))[0]
            elf = os.path.join(tmp, name + ".elf")
            try:
                simfw.link([src], elf, ["-I", UNIT], objdir=tmp)
            except RuntimeError as e:
                fail(f"{name}: riscv32 build failed\n{e}")
                continue
            p = sim_exec(elf, sd=sd)
            with open(os.path.splitext(src)[0] + ".expected", "rb") as f:
                want = f.read()
            if p.returncode != 0 or p.stdout != want:
                why = f"exit status {p.returncode}" if p.returncode else "output differs from .expected"
                fail(f"{name}: {why}\n{p.stderr.decode(errors='replace')}"
                     f"{diff(want.decode(errors='replace'), p.stdout.decode(errors='replace'))}")
            else:
                print(f"  ok   {name}")


def parse_expect(src):
    with open(src) as f:
        m = EXPECT_RE.match(f.readline().strip())
    if not m:
        return None
    return m.group(1), [int(x) for x in m.group(2).split('|')], m.group(3).strip()


def mode_errors():
    gcc = find_rv_gcc()
    print(f"[errors] checking tests against {os.path.basename(gcc)}")
    with tempfile.TemporaryDirectory() as tmp:
        for src in error_tests():
            name = os.path.basename(src)
            exp = parse_expect(src)
            if not exp:
                fail(f"{name}: missing // EXPECT: header")
                continue
            stage, line, _ = exp
            r = run([gcc, "-std=gnu11", "-march=rv32imafc", "-mabi=ilp32f", "-c", src,
                     "-o", os.path.join(tmp, "e.o")])
            if stage == "compile":
                if r.returncode == 0:
                    fail(f"{name}: gcc accepted it, expected a compile error")
                    continue
                lines = [int(m.group(1)) for m in re.finditer(re.escape(os.path.basename(src)) + r":(\d+):\d+: (?:fatal )?error", r.stderr)]
                hit = sorted(set(line) & set(lines))
                if not hit:
                    fail(f"{name}: gcc errors at lines {lines}, header says {line}\n{r.stderr}")
                else:
                    print(f"  ok   {name} (compile error at line {hit[0]})")
            else:
                if r.returncode != 0:
                    fail(f"{name}: should compile cleanly and fail only at link\n{r.stderr}")
                else:
                    print(f"  ok   {name} (compiles; must fail at link)")


def mode_pcc(pcc_cmd, exec_cmd):
    if not pcc_cmd:
        sys.exit("pcc mode needs --pcc")
    with tempfile.TemporaryDirectory() as tmp:
        # modules go to <tmp>/sd/pcx: pcxrun opens them through the guest file system (/sd/pcx/...)
        sd = new_sd_dir(tmp)
        outdir = os.path.join(sd, "pcx")
        os.makedirs(outdir, exist_ok=True)
        if exec_cmd == "rvsim":
            build_host.ensure_built()
            # forward slashes: the command goes through shlex.split, which eats backslashes
            fwd = lambda p: p.replace("\\", "/")  # noqa: E731
            exec_cmd = (f'"{fwd(build_host.RVSIM_EXE)}" --sd "{fwd(sd)}" '
                        f'"{fwd(build_host.PCXRUN_ELF)}" "{{gout}}"')
        gpath = lambda p: "/sd/" + os.path.relpath(p, sd).replace("\\", "/")  # noqa: E731
        for src in unit_tests():
            name = os.path.splitext(os.path.basename(src))[0]
            out = os.path.join(outdir, name + ".pcx")
            r = run(shlex.split(pcc_cmd.format(src=src.replace("\\", "/"), out=out.replace("\\", "/"))))
            if r.returncode:
                fail(f"{name}: pcc failed\n{r.stdout}{r.stderr}")
                continue
            if not exec_cmd:
                print(f"  ok   {name} (compiled only)")
                continue
            p = run(shlex.split(exec_cmd.format(out=out.replace("\\", "/"), gout=gpath(out))), timeout=120)
            got = p.stdout.replace("\r\n", "\n")
            with open(os.path.splitext(src)[0] + ".expected", newline="") as f:
                want = f.read()
            if p.returncode or got != want:
                fail(f"{name}: run mismatch (status {p.returncode})\n{p.stderr}{diff(want, got)}")
            else:
                print(f"  ok   {name}")
        for src in error_tests():
            name = os.path.basename(src)
            stage, line, pattern = parse_expect(src)
            out = os.path.join(outdir, "err.pcx")
            r = run(shlex.split(pcc_cmd.format(src=src.replace("\\", "/"), out=out.replace("\\", "/"))))
            msg = r.stdout + r.stderr
            if r.returncode == 0:
                fail(f"{name}: pcc accepted it")
            elif "Traceback" in msg or "Segmentation" in msg or r.returncode < 0:
                fail(f"{name}: pcc crashed\n{msg}")
            elif not re.search(pattern, msg, re.I):
                fail(f"{name}: message does not match /{pattern}/\n{msg}")
            elif line != [0] and not any(re.search(rf":{n}:", msg) for n in line):
                fail(f"{name}: diagnostic not at line {line}\n{msg}")
            else:
                print(f"  ok   {name}")


def diff(want, got):
    import difflib
    return "".join(difflib.unified_diff(want.splitlines(True), got.splitlines(True), "expected", "got", n=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", nargs="?", default="all", choices=["all", "ref", "target", "errors", "simref", "pcc"])
    ap.add_argument("--update", action="store_true", help="rewrite .expected files from the host build")
    ap.add_argument("--pcc", help='compiler command, e.g. "build/pcc {src} -o {out}"')
    ap.add_argument("--exec", dest="exec_cmd", help='module runner command, e.g. "build/rv32emu {out}"')
    a = ap.parse_args()
    if a.mode in ("all", "ref"):
        mode_ref(a.update)
    if a.mode in ("all", "target"):
        mode_target()
    if a.mode in ("all", "errors"):
        mode_errors()
    if a.mode in ("all", "simref"):
        mode_simref()
    if a.mode == "pcc":
        mode_pcc(a.pcc, a.exec_cmd)
    print(f"\n{failures} failure(s)")
    sys.exit(min(failures, 125))


if __name__ == "__main__":
    main()
