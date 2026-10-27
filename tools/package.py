#!/usr/bin/env python3
"""Package PicoCalc Studio for one board: build/picocalc-studio-<version>-<board>.zip holding the
picocalc-studio/ SD folder (headers, SDK sources, examples, tests, app.ini, icon.bmp) with the
firmware in it. Users unzip it into the root of the SD card. Build the firmware first
(tools\\build_rp2350.ps1 or tools\\build.ps1).

  python tools/package.py rp2350
  python tools/package.py esp32p4

app.ini in the zip names only that board, so PicoMultiLoader's checker passes on the folder.
"""
import re
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SD_DIR = REPO / "sdcard" / "picocalc-studio"
BOARDS = {   # board: (firmware built by the build script, its name in the folder)
    "rp2350": (REPO / "build" / "picocalc_studio_rp2350" / "picocalc_studio.uf2", "picocalc_studio.uf2"),
    "esp32p4": (REPO / "build" / "picocalc_studio_p4" / "picocalc_studio.bin", "picocalc_studio.bin"),
}
SKIP_DIRS = {"projects", "apps"}           # the user's own files, if any were left in the repo copy
SKIP_FILES = {"ide.cfg", "results.txt"}


def define(text, name):
    m = re.search(r'#define\s+%s\s+"([^"]*)"' % name, text)
    if not m:
        sys.exit("version.h: no %s" % name)
    return m.group(1)


def app_ini(board):
    """app.ini for one board: the other boards' binary.<board> lines dropped, board = <board>."""
    out = []
    for line in (SD_DIR / "app.ini").read_text().splitlines():
        key = line.split("=", 1)[0].strip().lower() if "=" in line and not line.lstrip().startswith(";") else ""
        if key.startswith("binary.") and key != "binary." + board:
            continue
        if key == "board":
            line = re.sub(r"=.*", "= " + board, line)
        out.append(line)
    return "\r\n".join(out) + "\r\n"


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in BOARDS:
        sys.exit(__doc__)
    board = sys.argv[1]
    firmware, fw_name = BOARDS[board]
    if not firmware.exists():
        sys.exit("no %s: build it first" % firmware)
    version = define((REPO / "firmware" / "picocalc_studio" / "version.h").read_text(), "PCS_VERSION")
    ini = app_ini(board)
    m = re.search(r"^\s*version\s*=\s*(\S+)", ini, re.M | re.I)
    if not m or m.group(1) != version:
        sys.exit("app.ini version %s != version.h %s" % (m and m.group(1), version))

    out = REPO / "build" / ("picocalc-studio-%s-%s.zip" % (version, board))
    n = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(SD_DIR.rglob("*")):
            rel = f.relative_to(SD_DIR)
            if not f.is_file() or rel.parts[0] in SKIP_DIRS or f.name in SKIP_FILES or f.name == "app.ini":
                continue
            z.write(f, "picocalc-studio/" + rel.as_posix())
            n += 1
        z.writestr("picocalc-studio/app.ini", ini)
        z.write(firmware, "picocalc-studio/" + fw_name)
    print("%s: %d files + app.ini + %s (%d KB)" % (out, n, fw_name, out.stat().st_size // 1024))


if __name__ == "__main__":
    main()
