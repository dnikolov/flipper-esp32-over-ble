# Builds and runs the host-native flipper codec test with MSVC (cl.exe).
# Locates cl.exe via vswhere; no gcc/MinGW is assumed to be on PATH (see docs/PLAN.md
# step 3 implementation decisions).
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
$flipperDir = Join-Path $repoRoot "flipper"
$vectorsDir = Join-Path $repoRoot "tests\vectors"
$outDir = Join-Path $scriptDir "build"

if(-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

$sources = @(
    (Join-Path $scriptDir "test_flipper_codec.c"),
    (Join-Path $flipperDir "framing.c"),
    (Join-Path $flipperDir "cbor_primitives.c"),
    (Join-Path $flipperDir "cbor_records.c"),
    (Join-Path $flipperDir "cbor_wifi_scan.c"),
    (Join-Path $flipperDir "cbor_ble_scan.c"),
    (Join-Path $flipperDir "cbor_wardriving.c"),
    (Join-Path $flipperDir "wardriving_csv.c")
) -join " "

$includeDirs = "/I `"$flipperDir`" /I `"$vectorsDir`""
$exePath = Join-Path $outDir "test_flipper_codec.exe"

$cmd = "call `"$vcvarsall`" >nul && cl.exe /nologo /W4 /utf-8 $includeDirs /Fe:`"$exePath`" /Fo:`"$outDir\\`" $sources"

Write-Host "Building host test..."
cmd.exe /c $cmd
if($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Running host test..."
& $exePath
exit $LASTEXITCODE
