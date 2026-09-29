# Builds and runs the host-native nmea_parser.c test with MSVC (cl.exe). Mirrors build.ps1's
# vswhere/vcvars64 setup. location.c itself now depends on driver/uart.h and FreeRTOS (the
# real GPS driver, docs/PLAN.md "Real GPS driver, wardriving fix-dependency, and real
# wardriving-record timestamps") and is not host-buildable -- same treatment as
# wardriving_log.c/wardriving_record_format.c's split; this script (kept under its original
# name/location) now targets nmea_parser.c, the pure sentence-parsing module underneath it.
# nmea_parser.c now lives in components/feb_wardriving/ (shared across esp32/esp32c5/heltec
# since docs/SOURCE_SPLIT.md stage E1), not under esp32/main/.
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
$wardrivingDir = Join-Path $repoRoot "components\feb_wardriving"
$outDir = Join-Path $scriptDir "build"

if(-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

$sources = @(
    (Join-Path $scriptDir "test_location.c"),
    (Join-Path $wardrivingDir "nmea_parser.c")
) -join " "

$includeDirs = "/I `"$wardrivingDir`""
$exePath = Join-Path $outDir "test_location.exe"

$cmd = "call `"$vcvarsall`" >nul && cl.exe /nologo /W4 /std:c11 $includeDirs /Fe:`"$exePath`" /Fo:`"$outDir\\`" $sources"

Write-Host "Building host test..."
cmd.exe /c $cmd
if($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Running host test..."
& $exePath
exit $LASTEXITCODE
