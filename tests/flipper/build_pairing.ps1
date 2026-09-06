# Builds and runs the host-native pairing crypto/codec test with MSVC (cl.exe).
# Mirrors build.ps1's approach (see that file for the vswhere/vcvars64 rationale); kept as
# a separate script rather than folded into build.ps1 so each host test binary can be run
# independently and their source lists stay easy to audit.
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
    (Join-Path $scriptDir "test_pairing.c"),
    (Join-Path $flipperDir "pairing_crypto.c"),
    (Join-Path $flipperDir "pairing.c"),
    (Join-Path $flipperDir "cbor_codec.c")
) -join " "

$includeDirs = "/I `"$flipperDir`" /I `"$vectorsDir`""
$exePath = Join-Path $outDir "test_pairing.exe"

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
