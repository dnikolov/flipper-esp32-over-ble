# Builds and runs the host-native esp32 runtime-session test with MSVC (cl.exe).
# Sibling to build_pairing.ps1 (not folded into it), same pattern used for step 5:
# this binary pulls in ESP-IDF's vendored mbedtls GCM/AES/block-cipher sources on
# top of pairing_crypto.c's existing sha256/md/hkdf/bignum dependency -- see
# docs/PLAN.md step 6. No gcc/MinGW is assumed to be on PATH.
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
$vectorsDir = Join-Path $repoRoot "tests\vectors"
$outDir = Join-Path $scriptDir "build"

$idfPath = $env:IDF_PATH
if(-not $idfPath) {
    $idfPath = "C:\Users\Deyan\esp\esp-idf"
}
$mbedtlsRoot = Join-Path $idfPath "components\mbedtls\mbedtls"
$mbedtlsInclude = Join-Path $mbedtlsRoot "include"
$mbedtlsLibrary = Join-Path $mbedtlsRoot "library"
if(-not (Test-Path $mbedtlsInclude)) {
    throw "ESP-IDF mbedtls checkout not found at $mbedtlsInclude -- set IDF_PATH."
}

if(-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

$sources = @(
    (Join-Path $scriptDir "test_session.c"),
    (Join-Path $esp32Dir "session_crypto.c"),
    (Join-Path $esp32Dir "session.c"),
    (Join-Path $esp32Dir "pairing_crypto.c"),
    (Join-Path $esp32Dir "pairing.c"),
    (Join-Path $esp32Dir "cbor_codec.c"),
    (Join-Path $mbedtlsLibrary "sha256.c"),
    (Join-Path $mbedtlsLibrary "md.c"),
    (Join-Path $mbedtlsLibrary "hkdf.c"),
    (Join-Path $mbedtlsLibrary "bignum.c"),
    (Join-Path $mbedtlsLibrary "bignum_core.c"),
    (Join-Path $mbedtlsLibrary "constant_time.c"),
    (Join-Path $mbedtlsLibrary "platform_util.c"),
    (Join-Path $mbedtlsLibrary "gcm.c"),
    (Join-Path $mbedtlsLibrary "block_cipher.c"),
    (Join-Path $mbedtlsLibrary "aes.c")
) -join " "

$includeDirs = "/I `"$esp32Dir`" /I `"$vectorsDir`" /I `"$scriptDir`" /I `"$mbedtlsInclude`" /I `"$mbedtlsLibrary`""
$defines = "/DMBEDTLS_CONFIG_FILE=`"\`"mbedtls_test_config.h\`"`""
$exePath = Join-Path $outDir "test_session.exe"

$cmd = "call `"$vcvarsall`" >nul && cl.exe /nologo /W3 /std:c11 $defines $includeDirs /Fe:`"$exePath`" /Fo:`"$outDir\\`" $sources"

Write-Host "Building host session test..."
cmd.exe /c $cmd
if($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Running host session test..."
& $exePath
exit $LASTEXITCODE
