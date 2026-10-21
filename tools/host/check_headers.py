#!/usr/bin/env python3
"""
Verify the PicoCalc C headers (sdcard/pico-dev/include) against the firmware's newlib.

  python tools/host/check_headers.py [-v]

1. layout   Generates one C file of checks (sizeof/_Alignof/offsetof of every fixed-layout type,
            the exact C type of typedefs, struct members and integer macros via _Generic, and the
            value of every numeric/string macro our headers define in limits/stdint/inttypes/errno/
            fcntl/sys/stat/stdio/stdlib/unistd/signal/dirent/math/float/time). It is compiled to
            assembly twice with riscv32-esp-elf-gcc: against newlib's own headers, and with
            -nostdinc against ours. Each check is a global initialised with the value; the values
            are read back from the .s files and compared.
2. protos   Every function prototype in our headers is re-declared after newlib's headers; gcc
            rejects any conflicting declaration.
3. headers  Each header compiled alone with -nostdinc -std=c11 -Wall -Wextra -Werror (twice: once
            normally, once with __chibicc__ defined to exercise the compiler's stdarg path).
4. tests    tests/unit/*.c and tests/projects/*/*.c pass -fsyntax-only against our headers only
            (-Werror=implicit-function-declaration); functions they use that the firmware does
            not export are listed.

Exit status = number of failures.
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
INC = os.path.join(ROOT, "sdcard", "pico-dev", "include")
EXPORTS = os.path.join(ROOT, "firmware", "picocalc_studio", "src", "rt", "exports.txt")
RV_GCC_GLOB = os.path.expanduser(
    r"~/.emu6502-p4/data/packages/esp32/tools/esp-rv32/*/bin/riscv32-esp-elf-gcc*")
ARCH = ["-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f"]
OURS = ["-nostdinc", "-isystem", INC, "-funsigned-char", "-std=c11"]
NEWLIB = ["-std=gnu11"]

HEADERS = ["assert.h", "ctype.h", "dirent.h", "errno.h", "fcntl.h", "float.h", "inttypes.h",
           "iso646.h", "limits.h", "math.h", "setjmp.h", "signal.h", "stdalign.h", "stdarg.h",
           "stdbool.h", "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "stdnoreturn.h", "string.h",
           "strings.h", "time.h", "unistd.h", "sys/stat.h", "sys/types.h", "sys/reent.h"]
# headers whose object-like macros are value-checked against newlib
MACRO_HEADERS = ["limits.h", "stdint.h", "inttypes.h", "errno.h", "fcntl.h", "sys/stat.h",
                 "stdio.h", "stdlib.h", "unistd.h", "signal.h", "dirent.h", "math.h", "float.h",
                 "time.h"]
MACRO_SKIP = {"NULL", "stdin", "stdout", "stderr", "errno", "_REENT", "SIG_DFL", "SIG_IGN",
              "SIG_ERR", "M_TWOPI"}
# deliberate differences: our long double is double (gcc: 128-bit); DECIMAL_DIG follows it
MACRO_SKIP_RE = re.compile(r"^(st_[amc]time$|LDBL_|HUGE_VALL$|DECIMAL_DIG$)")

TYPEID = """
#define TYPEID(x) _Generic((x), _Bool: 1, char: 2, signed char: 3, unsigned char: 4, \\
    short: 5, unsigned short: 6, int: 7, unsigned int: 8, long: 9, unsigned long: 10, \\
    long long: 11, unsigned long long: 12, float: 13, double: 14, long double: 15, default: 0)
#define SIZE_ALIGN_ID(T) S(sizeof(T)) S(_Alignof(T)) S(TYPEID((T)0))
"""

ARITH_TYPES = """
size_t ptrdiff_t wchar_t int8_t uint8_t int16_t uint16_t int32_t uint32_t int64_t uint64_t
int_least8_t uint_least8_t int_least16_t uint_least16_t int_least32_t uint_least32_t
int_least64_t uint_least64_t int_fast8_t uint_fast8_t int_fast16_t uint_fast16_t int_fast32_t
uint_fast32_t int_fast64_t uint_fast64_t intptr_t uintptr_t intmax_t uintmax_t
time_t clock_t off_t ssize_t mode_t dev_t ino_t nlink_t uid_t gid_t pid_t blksize_t blkcnt_t
key_t suseconds_t useconds_t clockid_t timer_t id_t daddr_t fsblkcnt_t fsfilcnt_t
u_char u_short u_int u_long ushort uint ulong fpos_t sig_atomic_t float_t double_t
""".split()

STRUCTS = {
    "struct stat": "st_dev st_ino st_mode st_nlink st_uid st_gid st_rdev st_size st_atim st_mtim "
                   "st_ctim st_blksize st_blocks st_spare4",
    "struct timespec": "tv_sec tv_nsec",
    "struct dirent": "d_ino d_type d_name",
    "struct tm": "tm_sec tm_min tm_hour tm_mday tm_mon tm_year tm_wday tm_yday tm_isdst",
    "struct _reent": "_errno _stdin _stdout _stderr",
    "div_t": "quot rem",
    "ldiv_t": "quot rem",
    "lldiv_t": "quot rem",
    "imaxdiv_t": "quot rem",
}
SIZE_ONLY = ["jmp_buf", "va_list", "DIR *", "FILE *", "struct stat", "struct _reent"]

failures = 0


def fail(msg):
    global failures
    failures += 1
    print("  FAIL " + msg)


def find_gcc():
    hits = sorted(h for h in glob.glob(RV_GCC_GLOB) if re.search(r"gcc(\.exe)?$", h))
    if not hits:
        sys.exit("riscv32-esp-elf-gcc not found under ~/.emu6502-p4")
    return hits[-1]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def strip_comments(src):
    return re.sub(r"/\*.*?\*/|//[^\n]*", " ", src, flags=re.S)


def object_macros(header):
    """(name, value) of the object-like #defines of one of our headers."""
    src = strip_comments(open(os.path.join(INC, header)).read())
    src = src.replace("\\\n", " ")
    out = []
    for m in re.finditer(r"^\s*#\s*define\s+([A-Za-z_]\w*)(?![\w(])[ \t]*(.*)$", src, re.M):
        name, val = m.group(1), m.group(2).strip()
        if not val or name.startswith("_") and name.endswith("_H"):
            continue
        out.append((name, val))
    return out


def classify(name, val, all_macros):
    if name in MACRO_SKIP or MACRO_SKIP_RE.match(name) or name.endswith("_DECLARED"):
        return None
    v = val
    for _ in range(4):   # resolve aliases like PRIdLEAST8 -> PRId8, S_IFDIR -> _IFDIR
        if re.fullmatch(r"[A-Za-z_]\w*", v) and v in all_macros:
            v = all_macros[v]
        else:
            break
    if v.startswith('"'):
        return "str"
    if re.fullmatch(r"[A-Za-z_]\w*", v):
        return None
    if re.search(r"\d\.\d|\de[+-]?\d|\.\d|\d\.", v) and not v.startswith("0x"):
        return "dbl"
    return "int"


def build_checks():
    """List of (label, kind, expr); kind in int/dbl/str."""
    checks = []
    S = lambda label, e: checks.append((label, "int", e))  # noqa: E731
    for t in ARITH_TYPES:
        S(f"sizeof({t})", f"sizeof({t})")
        S(f"_Alignof({t})", f"_Alignof({t})")
        S(f"typeid({t})", f"TYPEID(({t})0)")
    for t in SIZE_ONLY:
        S(f"sizeof({t})", f"sizeof({t})")
        S(f"_Alignof({t})", f"_Alignof({t})")
    for st, fields in STRUCTS.items():
        S(f"sizeof({st})", f"sizeof({st})")
        for f in fields.split():
            S(f"offsetof({st}, {f})", f"__builtin_offsetof({st}, {f})")
            S(f"sizeof({st}.{f})", f"sizeof((({st} *)0)->{f})")
            S(f"typeid({st}.{f})", f"TYPEID((({st} *)0)->{f})")
    S("sizeof(va_list arg slot)", "sizeof(((va_list *)0)[0])")
    all_macros = {}
    per_header = []
    for h in MACRO_HEADERS:
        ms = object_macros(h)
        per_header.append((h, ms))
        all_macros.update(dict(ms))
    seen = set()
    for h, ms in per_header:
        for name, val in ms:
            if name in seen:
                continue
            seen.add(name)
            kind = classify(name, val, all_macros)
            if kind == "int":
                checks.append((name, "int", name))
                checks.append((f"typeid({name})", "int", f"TYPEID({name})"))
            elif kind:
                checks.append((name, kind, name))
                if kind == "dbl":
                    checks.append((f"typeid({name})", "int", f"TYPEID({name})"))
    # macros the tests / task require to exist
    for name in ["EOF", "BUFSIZ", "FOPEN_MAX", "FILENAME_MAX", "SEEK_SET", "SEEK_CUR", "SEEK_END",
                 "RAND_MAX", "EXIT_SUCCESS", "EXIT_FAILURE", "DT_REG", "DT_DIR", "S_IFMT",
                 "S_IFDIR", "S_IFREG", "O_RDONLY", "O_WRONLY", "O_RDWR", "O_APPEND", "O_CREAT",
                 "O_TRUNC", "O_EXCL", "ENOENT", "ENOTEMPTY", "CLOCKS_PER_SEC"]:
        if name not in seen:
            checks.append((name, "int", name))
    return checks


def gen_source(checks, includes):
    lines = [f"#include <{h}>" for h in includes]
    lines.append(TYPEID)
    for i, (label, kind, expr) in enumerate(checks):
        m = re.fullmatch(r"(?:TYPEID\()?([A-Za-z_]\w*)\)?", expr)
        guard = m is not None
        if guard:
            lines.append(f"#ifdef {m.group(1)}")
        if kind == "int":
            lines.append(f"long long v_{i} = (long long)({expr});")
        elif kind == "dbl":
            lines.append(f"double v_{i} = ({expr});")
        else:
            lines.append(f"const char v_{i}[] = {expr};")
        if guard:
            lines.append("#endif")
    return "\n".join(lines) + "\n"


def parse_asm(text):
    """label -> bytes for every v_N data object in a gcc RISC-V .s file."""
    vals = {}
    cur = None
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r"^(v_\d+):$", s)
        if m:
            cur = m.group(1)
            vals[cur] = b""
            continue
        if cur is None:
            continue
        if re.match(r"^[A-Za-z_.$][\w.$]*:$", s) or s.startswith(".size") or s.startswith(".globl") \
                or s.startswith(".section") or s.startswith(".text") or s.startswith(".align") \
                or s.startswith(".type"):
            if not s.startswith(".align"):
                cur = None
            continue
        m = re.match(r"^\.(word|half|byte|dword)\s+(-?\w+)", s)
        if m:
            n = {"byte": 1, "half": 2, "word": 4, "dword": 8}[m.group(1)]
            vals[cur] += (int(m.group(2), 0) & ((1 << (8 * n)) - 1)).to_bytes(n, "little")
            continue
        m = re.match(r"^\.zero\s+(\d+)", s)
        if m:
            vals[cur] += bytes(int(m.group(1)))
            continue
        m = re.match(r'^\.(string|ascii)\s+"(.*)"$', s)
        if m:
            b = m.group(2).encode().decode("unicode_escape").encode("latin-1")
            vals[cur] += b + (b"\0" if m.group(1) == "string" else b"")
    return vals


def decode(kind, b):
    if b is None:
        return "<undefined>"
    if kind == "int":
        return int.from_bytes(b[:8], "little", signed=True)
    if kind == "dbl":
        import struct
        return repr(struct.unpack("<d", b[:8])[0]) + f" ({b[:8][::-1].hex()})"
    return repr(b.rstrip(b"\0").decode("latin-1"))


def compile_values(gcc, checks, flags, includes, tmp, tag):
    src = os.path.join(tmp, f"layout_{tag}.c")
    asm = os.path.join(tmp, f"layout_{tag}.s")
    open(src, "w").write(gen_source(checks, includes))
    r = run([gcc, *ARCH, *flags, "-S", "-O0", "-fno-common", "-w", src, "-o", asm])
    if r.returncode:
        fail(f"layout check file does not compile against {tag} headers:\n{r.stderr[:3000]}")
        return None
    vals = parse_asm(open(asm).read())
    return [decode(kind, vals.get(f"v_{i}")) for i, (_, kind, _) in enumerate(checks)]


def check_layout(gcc, tmp, verbose):
    print("[layout] values compiled against newlib vs our headers")
    checks = build_checks()
    includes = [h for h in HEADERS if h not in ("assert.h", "sys/reent.h")]
    ref = compile_values(gcc, checks, NEWLIB, includes + ["sys/reent.h"], tmp, "newlib")
    ours = compile_values(gcc, checks, OURS, includes, tmp, "ours")
    if ref is None or ours is None:
        return
    bad = 0
    for (label, _, _), a, b in zip(checks, ref, ours):
        if a == "<undefined>" and label in ("MATH_ERREXCEPT",):
            continue
        if a != b:
            bad += 1
            fail(f"{label}: newlib {a}  ours {b}")
        elif verbose:
            print(f"  ok   {label} = {a}")
    print(f"  {len(checks) - bad}/{len(checks)} checks match")


def prototypes(header):
    src = strip_comments(open(os.path.join(INC, header)).read()).replace("\\\n", " ")
    src = re.sub(r"^\s*#.*$", "", src, flags=re.M)
    out, depth, cur = [], 0, ""
    for ch in src:
        cur += ch
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                cur = ""     # struct body or inline function: skip
        elif ch == ";" and depth == 0:
            d = " ".join(cur.split())
            cur = ""
            if "(" in d and not d.startswith(("typedef", "static", "struct", "}")):
                m = re.search(r"([A-Za-z_]\w*)\s*\(", d.replace("(*", "( *"))
                if m:
                    out.append((m.group(1), d))
    return out


def check_protos(gcc, tmp):
    print("[protos] our prototypes re-declared after newlib's headers")
    lines = [f"#include <{h}>" for h in HEADERS if h != "sys/reent.h"]
    lines.append("#include <sys/reent.h>")
    names = []
    for h in HEADERS:
        for name, decl in prototypes(h):
            names.append(name)
            lines.append(f"#undef {name}")
            lines.append(decl)
    src = os.path.join(tmp, "protos.c")
    open(src, "w").write("\n".join(lines) + "\n")
    r = run([gcc, *ARCH, *NEWLIB, "-fsyntax-only", "-Werror", src])
    if r.returncode:
        fail("prototype conflicts with newlib:\n" + r.stderr[:4000])
    else:
        print(f"  {len(names)} prototypes compatible")
    return names


def check_headers(gcc, tmp):
    print("[headers] each header alone, -nostdinc -std=c11 -Wall -Wextra -Werror")
    n = 0
    for h in HEADERS + ["picocalc.h"]:
        for extra in ([], ["-D__chibicc__"]):
            src = os.path.join(tmp, "h.c")
            body = f"#include <{h}>\n#include <{h}>\nint pc_header_check_dummy;\n"
            if h == "stdarg.h":
                body += ("int f(int n, ...) { va_list ap, aq; va_start(ap, n); va_copy(aq, ap);\n"
                         "  double d = va_arg(ap, double); long long l = va_arg(aq, long long);\n"
                         "  struct big { int a[5]; } s = va_arg(ap, struct big);\n"
                         "  va_end(ap); va_end(aq); return n + (int)d + (int)l + s.a[0]; }\n")
                if extra:
                    body = "static char *__va_area__;\n" + body
            if h == "math.h":
                body += ("int g(float f, double d) { return isnan(f) + isinf(d) + isfinite(f) +\n"
                         "  signbit(f) + signbit(d) + fpclassify(f) + fpclassify(d) + isnormal(d) +\n"
                         "  isless(f, d) + (INFINITY > HUGE_VAL) + (NAN != NAN); }\n")
            if h == "assert.h":
                body += "void g(int x) { assert(x > 0); }\n"
            open(src, "w").write(body)
            r = run([gcc, *ARCH, *OURS, "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                     *extra, src])
            n += 1
            if r.returncode:
                fail(f"{h} {' '.join(extra)}:\n{r.stderr[:2000]}")
    print(f"  {n} compiles done")


def exported():
    names = set()
    for line in open(EXPORTS):
        line = line.split("#")[0].strip()
        if line:
            names.add(line.split("=")[0])
    return names


def check_tests(gcc, tmp, protos):
    print("[tests] tests/unit + tests/projects, -fsyntax-only against our headers only")
    srcs = sorted(glob.glob(os.path.join(ROOT, "tests", "unit", "t*.c")))
    srcs += sorted(glob.glob(os.path.join(ROOT, "tests", "projects", "*", "*.c")))
    exp = exported()
    picocalc = set(re.findall(r"\b([a-z_]\w*)\s*\(", strip_comments(
        open(os.path.join(INC, "picocalc.h")).read())))
    used_missing = {}
    for src in srcs:
        rel = os.path.relpath(src, ROOT)
        r = run([gcc, *ARCH, *OURS, "-fsyntax-only", "-Werror=implicit-function-declaration",
                 "-I", os.path.dirname(src), src])
        if r.returncode:
            fail(f"{rel}:\n{r.stderr[:2000]}")
        # library functions used by the test that the firmware does not export
        r = run([gcc, *ARCH, *OURS, "-E", "-P", "-I", os.path.dirname(src), src])
        body = r.stdout
        for name in set(protos):
            if name not in exp and name not in picocalc and re.search(rf"\b{name}\s*\(", body):
                # declared + called somewhere other than its prototype
                calls = len(re.findall(rf"\b{name}\s*\(", body))
                if calls > 1:
                    used_missing.setdefault(name, []).append(rel)
    print(f"  {len(srcs)} files")
    for name, files in sorted(used_missing.items()):
        print(f"  NOTE {name}() is declared but not exported; used by {', '.join(files)}")


def main():
    verbose = "-v" in sys.argv
    gcc = find_gcc()
    with tempfile.TemporaryDirectory() as tmp:
        check_layout(gcc, tmp, verbose)
        protos = check_protos(gcc, tmp)
        check_headers(gcc, tmp)
        check_tests(gcc, tmp, protos)
    exp = exported()
    missing = sorted({p for p in protos if p not in exp})
    if missing:
        print("[exports] declared in our headers but not in exports.txt: " + ", ".join(missing))
    print("OK" if failures == 0 else f"{failures} failure(s)")
    return failures


if __name__ == "__main__":
    sys.exit(main())
