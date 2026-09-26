# Builds and runs the host-native esp32 codec test with MSVC (cl.exe).
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
$sharedDir = Join-Path $repoRoot "components\feb_protocol"
$vectorsDir = Join-Path $repoRoot "tests\vectors"
$outDir = Join-Path $scriptDir "build"

if(-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

$sources = @(
    (Join-Path $scriptDir "test_framing_cbor.c"),
    (Join-Path $sharedDir "framing.c"),
    (Join-Path $sharedDir "cbor_primitives.c"),
    (Join-Path $sharedDir "cbor_records.c"),
    (Join-Path $sharedDir "cbor_wifi_scan.c"),
    (Join-Path $sharedDir "cbor_ble_scan.c"),
    (Join-Path $sharedDir "cbor_wardriving.c"),
    (Join-Path $sharedDir "cbor_gps.c"),
    (Join-Path $sharedDir "cbor_meshcore.c")
) -join " "

$includeDirs = "/I `"$sharedDir`" /I `"$vectorsDir`""
$exePath = Join-Path $outDir "test_framing_cbor.exe"

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
