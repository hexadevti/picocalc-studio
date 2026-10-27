#!/usr/bin/env python3
"""
Screenshots of PicoCalc Studio and its examples, for the docs (docs/screenshots/*.png).

The IDE (src/ide, src/ui, the compiler in src/cc and the SDK's gfx/kbd/sys) is built natively with
the host gcc against a PC HAL (host_hal.cpp: an in-memory LCD, keys from a script). Examples are
built the same way, with gcc instead of the on-device compiler. Every run gets a fresh copy of
sdcard/ as its SD card. Needs MinGW gcc/g++ and Pillow.

  python tools/host/shots/shots.py [name...]     # all shots, or the named ones
"""
import os
import shutil
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
FW = os.path.join(ROOT, "firmware", "picocalc_studio", "src")
OUT = os.path.join(ROOT, "build", "shots")
DOCS = os.path.join(ROOT, "docs", "screenshots")
SCALE = 2

FLAGS = ["-O2", "-w", "-include", os.path.join(HERE, "shim.h"), "-I", os.path.join(HERE, "include"),
         "-I", os.path.join(FW, "sdk")]
SDK = [os.path.join(FW, "sdk", f) for f in ("gfx.cpp", "kbd.cpp", "sys.cpp")]
SDK += [os.path.join(HERE, f) for f in ("host_hal.cpp", "host_snd.cpp")]
IDE = [os.path.join(FW, "ide", f) for f in ("ide.cpp", "doc.cpp", "editor.cpp", "complete.cpp")]
IDE += [os.path.join(FW, "ui", "ui.cpp"), os.path.join(HERE, "host_rt.cpp"), os.path.join(HERE, "ide_main.cpp")]
CC = sorted(os.path.join(FW, "cc", f) for f in os.listdir(os.path.join(FW, "cc")) if f.endswith(".c"))


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit("failed: " + " ".join(cmd[:3]) + " ...\n" + r.stdout + r.stderr)
    return r


def compile_all(srcs, objdir):
    os.makedirs(objdir, exist_ok=True)
    objs = []
    for s in srcs:
        o = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + ".o")
        if not os.path.exists(o) or os.path.getmtime(o) < os.path.getmtime(s):
            cpp = s.endswith(".cpp")
            run(["g++" if cpp else "gcc", *(["-std=gnu++17"] if cpp else ["-std=gnu11"]), *FLAGS,
                 *(["-I", os.path.join(FW, "cc")] if not cpp else []), "-c", s, "-o", o])
        objs.append(o)
    return objs


def build_ide():
    exe = os.path.join(OUT, "ide.exe")
    objs = compile_all(SDK + IDE, os.path.join(OUT, "obj")) + compile_all(CC, os.path.join(OUT, "obj", "cc"))
    run(["g++", *objs, "-o", exe])
    return exe


def build_example(name):
    exe = os.path.join(OUT, name + ".exe")
    src = os.path.join(ROOT, "sdcard", "picocalc-studio", "examples", name)
    srcs = [os.path.join(src, f) for f in sorted(os.listdir(src)) if f.endswith(".c")]
    objs = compile_all(SDK + [os.path.join(HERE, "app_main.cpp")], os.path.join(OUT, "obj"))
    objs += compile_all(srcs, os.path.join(OUT, "obj", name))
    run(["g++", *objs, "-o", exe])
    return exe


def stage_sd(name, projects):
    """A fresh SD card: sdcard/ plus the given examples copied in as the user's own projects."""
    sd = os.path.join(OUT, "sd", name)
    shutil.rmtree(sd, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "sdcard"), sd)
    for p in projects:
        shutil.copytree(os.path.join(sd, "picocalc-studio", "examples", p),
                        os.path.join(sd, "picocalc-studio", "projects", p))
    return sd


# The IDE scripts start at the project list (no ide.cfg yet): + New project, + New from example,
# -- My files --, then the projects below, then -- Examples --.
MY_PROJECTS = ["grapher", "snake", "tetris"]

IDE_SHOTS = {
    "ide-projects": """
wait 300
shot ide-projects
""",
    "ide-editor": """
key DOWN
key ENTER
ctrl g
type 66
key ENTER
key DOWN 12
key END
shot ide-editor
""",
    "ide-complete": """
key DOWN
key ENTER
ctrl g
type 83
key ENTER
key END
key ENTER
type gfx_fill
ctrl SPACE
key DOWN
shot ide-complete
""",
    "ide-build": """
key DOWN
key ENTER
key F3
shot ide-build
""",
    "ide-error": """
key DOWN
key ENTER
ctrl g
type 100
key ENTER
key END
key LEFT
type  +
wait 300
key F3
wait 300
shot ide-error
""",
    "ide-files": """
key DOWN 2
key ENTER
key ESC
shot ide-files
""",
    "ide-decl": """
key DOWN
key ENTER
ctrl g
type 70
key ENTER
key RIGHT 14
key F9
wait 300
shot ide-decl
""",
    "ide-help": """
key DOWN
key ENTER
key F1
shot ide-help
key ENTER
wait 300
shot ide-keys
""",
}

# Examples: keys and waits for each (they run in real time).
APP_SHOTS = {
    "asteroids": "wait 1500\nkey ENTER\nwait 2500\nshot app-asteroids\n",
    "demoscene": "wait 9000\nshot app-demoscene\n",
    "grapher": "wait 2500\nshot app-grapher\n",
    "kart": "wait 1500\nkey ENTER\nwait 3500\nhold UP\nwait 2500\nshot app-kart\n",
    "mandelbrot": "wait 6000\nshot app-mandelbrot\n",
    "raycaster": "wait 1500\nkey UP\nkey UP\nwait 1500\nshot app-raycaster\n",
    "solids": "wait 2500\nshot app-solids\n",
    "starfighter": "wait 1500\nkey ENTER\nwait 2000\nhold SPACE\nwait 2500\nshot app-starfighter\n",
    "tetris": "wait 1500\nkey ENTER\nwait 300\n" + "".join(f"key {d} {n}\nkey SPACE\nwait 250\n" for d, n in [("LEFT", 4), ("RIGHT", 4), ("LEFT", 1), ("UP", 1), ("RIGHT", 2), ("LEFT", 2), ("RIGHT", 4), ("LEFT", 3)]) + "wait 1200\nshot app-tetris\n",
    "voxel": "wait 1500\nkey ENTER\nwait 3000\nshot app-voxel\n",
}


def shoot(exe, name, script, projects=()):
    sd = stage_sd(name, projects)
    raw = os.path.join(OUT, "raw")
    os.makedirs(raw, exist_ok=True)
    path = os.path.join(OUT, name + ".script")
    with open(path, "w") as f:
        f.write(script.lstrip())
    env = dict(os.environ, PICOCALC_SD=sd, PICOCALC_REPO=ROOT, SHOT_SCRIPT=path, SHOT_DIR=raw)
    r = subprocess.run([exe], env=env, capture_output=True, text=True, timeout=180)
    shots = [l.split("shot: ", 1)[1] for l in r.stderr.splitlines() if l.startswith("shot: ")]
    if r.returncode or not shots:
        sys.exit(f"{name}: exit {r.returncode}\n{r.stderr[-2000:]}")
    os.makedirs(DOCS, exist_ok=True)
    for ppm in shots:
        png = os.path.join(DOCS, os.path.splitext(os.path.basename(ppm))[0] + ".png")
        im = Image.open(ppm)
        im.resize((im.width * SCALE, im.height * SCALE), Image.NEAREST).save(png, optimize=True)
        print("  " + os.path.relpath(png, ROOT))


def main():
    want = set(sys.argv[1:])
    pick = lambda n: not want or n in want
    if any(pick(n) for n in IDE_SHOTS):
        ide = build_ide()
        for name, script in IDE_SHOTS.items():
            if pick(name):
                shoot(ide, name, script, MY_PROJECTS)
    for name, script in APP_SHOTS.items():
        if pick(name):
            shoot(build_example(name), name, script)


if __name__ == "__main__":
    main()
