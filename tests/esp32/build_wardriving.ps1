# Builds and runs the host-native wardriving test with MSVC (cl.exe): covers
# wardriving_record_format.c (the pure, zero-ESP-IDF-dependency checksum/header-packing/
# eviction-ordering helpers underneath wardriving_log.c's raw-flash circular log,
# docs/PLAN.md step 8's wardriving-log persistence) and wardriving_validate.c (the pure
# interval-field validation/default-substitution slice of main.c's
# handle_wardriving_command()). Separate from build.ps1 (the framing/cbor codec test)
# since neither of these depends on the cbor codec at all. wardriving_log.c itself depends
# directly on esp_partition.h and is not host-buildable; see its header comment. No
# gcc/MinGW is assumed to be on PATH (see docs/PLAN.md step 3 implementation decisions,
# which this mirrors).
$ErrorActionPreference = "Stop"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if(-not (Test-Path $vswhere)) {
    $vswhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}
if(-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found; cannot locate Visual Studio installation."
}

$vsInstallPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vsInstallPath) {
    throw "No Visual Studio installation with VC.Tools found."
}

$vcvarsall = Join-Path $vsInstallPath "VC\Auxiliary\Build\vcvars64.bat"
if(-not (Test-Path $vcvarsall)) {
    throw "vcvars64.bat not found at $vcvarsall"
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..\..")
$esp32Dir = Join-Path $repoRoot "esp32\main"
$outDir = Join-Path $scriptDir "build"

if(-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

$sources = @(
    (Join-Path $scriptDir "test_wardriving_log.c"),
    (Join-Path $esp32Dir "wardriving_record_format.c"),
    (Join-Path $esp32Dir "wardriving_validate.c")
) -join " "

$includeDirs = "/I `"$esp32Dir`""
$exePath = Join-Path $outDir "test_wardriving_log.exe"

$cmd = "call `"$vcvarsall`" >nul && cl.exe /nologo /W4 /std:c11 $includeDirs /Fe:`"$exePath`" /Fo:`"$outDir\\`" $sources"

Write-Host "Building host wardriving_record_format test..."
cmd.exe /c $cmd
if($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Running host wardriving_record_format test..."
& $exePath
exit $LASTEXITCODE
