# Builds and runs the host-native session crypto/codec test with MSVC (cl.exe).
# Mirrors build_pairing.ps1's approach (see that file and build.ps1 for the vswhere/
# vcvars64 rationale); kept as a separate script rather than folded into either so each
# host test binary can be run independently and their source lists stay easy to audit.
#
# session_crypto.c (frozen contract flipper/session_crypto.h) calls the real Flipper
# firmware's furi_hal_crypto_gcm_encrypt_and_tag()/_decrypt_and_verify(), which only exist
# inside the pinned Unleashed SDK (STM32WB AES1 hardware peripheral) -- there is no host
# equivalent. host_shims/furi_hal_crypto.h + furi_hal_crypto_stub.c provide a host-only,
# from-scratch AES-256-GCM substitute for that one API surface so the real, unmodified
# flipper/session_crypto.c and flipper/session.c can be compiled and exercised against the
# shared vectors on a desktop host. See host_shims/furi_hal_crypto.h's top comment.
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
$hostShimsDir = Join-Path $scriptDir "host_shims"
$outDir = Join-Path $scriptDir "build"

if(-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

$sources = @(
    (Join-Path $scriptDir "test_session.c"),
    (Join-Path $hostShimsDir "furi_hal_crypto_stub.c"),
    (Join-Path $flipperDir "session_crypto.c"),
    (Join-Path $flipperDir "session.c"),
    (Join-Path $flipperDir "pairing_crypto.c"),
    (Join-Path $flipperDir "pairing.c"),
    (Join-Path $flipperDir "cbor_codec.c")
) -join " "

$includeDirs = "/I `"$hostShimsDir`" /I `"$flipperDir`" /I `"$vectorsDir`""
$exePath = Join-Path $outDir "test_session.exe"

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
