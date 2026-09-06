# Builds and runs the host-native esp32 pairing test with MSVC (cl.exe).
# Separate from build.ps1 (the framing/cbor codec test) because this binary pulls
# in a chunk of ESP-IDF's vendored mbedtls sources (bignum/sha256/md/hkdf) that the
# framing/cbor codec test has no need for -- see docs/SESSION_MEMORY.md's step 5
# esp32-developer entry for why. No gcc/MinGW is assumed to be on PATH (see
# docs/PLAN.md step 3 implementation decisions, which this mirrors).
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
    (Join-Path $scriptDir "test_pairing.c"),
    (Join-Path $esp32Dir "pairing_crypto.c"),
    (Join-Path $esp32Dir "pairing.c"),
    (Join-Path $esp32Dir "cbor_codec.c"),
    (Join-Path $mbedtlsLibrary "sha256.c"),
    (Join-Path $mbedtlsLibrary "md.c"),
    (Join-Path $mbedtlsLibrary "hkdf.c"),
    (Join-Path $mbedtlsLibrary "bignum.c"),
    (Join-Path $mbedtlsLibrary "bignum_core.c"),
    (Join-Path $mbedtlsLibrary "constant_time.c"),
    (Join-Path $mbedtlsLibrary "platform_util.c")
) -join " "

$includeDirs = "/I `"$esp32Dir`" /I `"$vectorsDir`" /I `"$scriptDir`" /I `"$mbedtlsInclude`" /I `"$mbedtlsLibrary`""
$defines = "/DMBEDTLS_CONFIG_FILE=`"\`"mbedtls_test_config.h\`"`""
$exePath = Join-Path $outDir "test_pairing.exe"

$cmd = "call `"$vcvarsall`" >nul && cl.exe /nologo /W3 /std:c11 $defines $includeDirs /Fe:`"$exePath`" /Fo:`"$outDir\\`" $sources"

Write-Host "Building host pairing test..."
cmd.exe /c $cmd
if($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Running host pairing test..."
& $exePath
exit $LASTEXITCODE
