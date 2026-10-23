# build_rp2350.ps1 - build PicoCalc Studio for the Pico 2 W (RP2350, RISC-V cores) with arduino-cli
# and arduino-pico, and copy the .uf2 to build\picocalc_studio_rp2350\. It never uploads: copy the
# .uf2 to the SD card and flash it from PicoMultiLoader.
#
#   tools\build_rp2350.ps1
param(
    [string]$Sketch = "firmware\picocalc_studio"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$sketchDir = Join-Path $repo $Sketch
$name = Split-Path -Leaf $sketchDir
$buildPath = Join-Path $env:USERPROFILE ".picostudio-build\$name-rp2350"   # short path: Windows 260-char limit
$outDir = Join-Path $repo "build\$name`_rp2350"

# arduino-pico lives in the default Arduino directories, not the P4 build's.
Remove-Item Env:ARDUINO_DIRECTORIES_DATA, Env:ARDUINO_DIRECTORIES_USER -ErrorAction SilentlyContinue
# The plain Pico 2 board: the W's radio is not used, and its lwIP/CYW43 stack would take ~70 KB of RAM.
$fqbn = "rp2040:rp2040:rpipico2:arch=riscv,os=freertos,flash=4194304_0,freq=150"

# The compat headers (freertos/*.h, esp_heap_caps.h, dirent.h) stand in for ESP-IDF's.
# build.extra_flags reaches C, C++ and assembly alike (the board leaves it empty).
$compatInc = Join-Path $sketchDir "src\hal\picocalc_rp2350\include"
$sdkInc = Join-Path $sketchDir "src\sdk"
$gnu = "-D_GNU_SOURCE"   # newlib: fopencookie, strcasestr
# libgcc and libm run from flash, not RAM (see the script).
$ldHook = Join-Path $repo "tools\rp2350_ldflash.py"
# Windows PowerShell turns redirected native stderr (every compiler warning) into errors under Stop.
$ErrorActionPreference = "Continue"
arduino-cli compile --fqbn $fqbn `
    --build-property "build.extra_flags=$gnu -I$compatInc -I$sdkInc" `
    --build-property "recipe.hooks.linking.prelink.9.pattern=`"{runtime.tools.pqt-python3.path}/python3`" -I `"$ldHook`" `"{build.path}/memmap_default.ld`"" `
    --build-path $buildPath $sketchDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force $outDir | Out-Null
Copy-Item (Join-Path $buildPath "$name.ino.uf2") (Join-Path $outDir "$name.uf2")
Copy-Item (Join-Path $buildPath "$name.ino.elf") (Join-Path $outDir "$name.elf")
Get-Item (Join-Path $outDir "$name.uf2") | Format-Table Name, Length
