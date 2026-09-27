# Builds and runs the host-native meshtastic_proto.c test with MSVC (cl.exe). Mirrors
# build_meshcore_proto.ps1's vswhere/vcvars64 setup, but also links ESP-IDF's vendored
# mbedtls AES module (same $env:IDF_PATH-relative approach as build_pairing.ps1/
# build_session.ps1) since meshtastic_proto.c uses mbedtls_aes_crypt_ctr() for its
# default-channel name decrypt (docs/PLAN.md's "Meshtastic Scan Capability -- Heltec Board
# (Phase 1)" design plan).
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
$heltecMainDir = Join-Path $repoRoot "heltec\main"
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
    (Join-Path $scriptDir "test_meshtastic_proto.c"),
    (Join-Path $heltecMainDir "meshtastic_proto.c"),
    (Join-Path $mbedtlsLibrary "aes.c"),
    (Join-Path $mbedtlsLibrary "constant_time.c"),
    (Join-Path $mbedtlsLibrary "platform_util.c")
) -join " "

$includeDirs = "/I `"$heltecMainDir`" /I `"$scriptDir`" /I `"$mbedtlsInclude`" /I `"$mbedtlsLibrary`""
$defines = "/DMBEDTLS_CONFIG_FILE=`"\`"mbedtls_test_config.h\`"`""
$exePath = Join-Path $outDir "test_meshtastic_proto.exe"

$cmd = "call `"$vcvarsall`" >nul && cl.exe /nologo /W4 /std:c11 $defines $includeDirs /Fe:`"$exePath`" /Fo:`"$outDir\\`" $sources"

Write-Host "Building host test..."
cmd.exe /c $cmd
if($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Running host test..."
& $exePath
exit $LASTEXITCODE
