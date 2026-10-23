# build.ps1 - build a PicoCalc ESP32-P4 sketch with arduino-cli (same settings as pico-386) and copy
# the app image to build\<name>_p4\. It never uploads: `arduino-cli upload` would also write the
# bootloader and partition table and wipe PicoMultiLoader. Copy the .bin to the SD card and flash
# it from the loader instead.
#
#   tools\build.ps1                                     # firmware\picocalc_studio
#   tools\build.ps1 firmware\spikes\spike_p0 -ProbeSlot # also write <name>_slot.bin with the probe patched in
#   tools\build.ps1 -Project hello                      # tests\projects\hello compiled into the firmware
#                                                       # by gcc: build\picocalc_studio_p4\hello.bin
param(
    [string]$Sketch = "firmware\picocalc_studio",
    [switch]$ProbeSlot,
    [string]$Project = ""
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$sketchDir = Join-Path $repo $Sketch
$name = Split-Path -Leaf $sketchDir
$buildPath = Join-Path $env:USERPROFILE ".picostudio-build\$name"   # short path: Windows 260-char limit
$outDir = Join-Path $repo "build\$name`_p4"
$outName = $name

# -Project: compile a test project's C sources into the firmware in place of the demo. They go to
# src\app (gitignored), which is emptied again afterwards.
$appDir = Join-Path $sketchDir "src\app"
if ($Project) {
    $projDir = Join-Path $repo "tests\projects\$Project"
    if (-not (Test-Path $projDir)) { throw "no project $projDir" }
    New-Item -ItemType Directory -Force $appDir | Out-Null
    Get-ChildItem $appDir -File | Remove-Item -Force
    Copy-Item (Join-Path $projDir "*.c"), (Join-Path $projDir "*.h") $appDir -ErrorAction SilentlyContinue
    $outName = $Project
    $buildPath = "$buildPath-$Project"
}

$env:ARDUINO_DIRECTORIES_DATA = Join-Path $env:USERPROFILE ".emu6502-p4\data"
$env:ARDUINO_DIRECTORIES_USER = Join-Path $env:USERPROFILE ".emu6502-p4\user"
$fqbn = "esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=16M,ChipVariant=postv3,PartitionScheme=huge_app,DebugLevel=error"

# Project sources say #include <picocalc.h>. The C++ flags need it too: arduino-cli's library
# detection preprocesses every file, .c included, with the C++ recipe.
$sdkInc = Join-Path $sketchDir "src\sdk"
try {
    arduino-cli compile --fqbn $fqbn `
        --build-property "compiler.optimization_flags=-O2" `
        --build-property "compiler.cpp.extra_flags=-DBOARD_PICOCALC_P4 -I$sdkInc" `
        --build-property "compiler.c.extra_flags=-DBOARD_PICOCALC_P4 -I$sdkInc" `
        --build-path $buildPath $sketchDir
    $rc = $LASTEXITCODE
} finally {
    if ($Project) { Get-ChildItem $appDir -File | Remove-Item -Force }
}
if ($rc -ne 0) { exit $rc }

New-Item -ItemType Directory -Force $outDir | Out-Null
Copy-Item (Join-Path $buildPath "$name.ino.bin") (Join-Path $outDir "$outName.bin")
Copy-Item (Join-Path $buildPath "$name.ino.elf") (Join-Path $outDir "$outName.elf")
python (Join-Path $repo "tools\host\esp_image.py") info (Join-Path $outDir "$outName.bin")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($ProbeSlot) {
    python (Join-Path $repo "tools\host\esp_image.py") probe-slot (Join-Path $outDir "$name.bin") `
        --elf (Join-Path $outDir "$name.elf") -o (Join-Path $outDir "${name}_slot.bin")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
Write-Host "output: $outDir"
