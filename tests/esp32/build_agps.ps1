# Builds and runs the host-native `agps` test with MSVC (cl.exe): covers the pure modules under
# components/feb_app_core (feb_agps_casic.c CASIC framing/AID-INI/ACK parser/feeder,
# feb_agps_blob.c chunk reassembly, feb_agps_cmd.c argument codec). Same vswhere/vcvars64 setup
# as build_wardriving.ps1.
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
$coreDir = Join-Path $repoRoot "components\feb_app_core"
$sharedDir = Join-Path $repoRoot "components\feb_protocol"
$outDir = Join-Path $scriptDir "build"

if(-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

$sources = @(
    (Join-Path $scriptDir "test_agps.c"),
    (Join-Path $coreDir "feb_agps_casic.c"),
    (Join-Path $coreDir "feb_agps_blob.c"),
    (Join-Path $coreDir "feb_agps_cmd.c"),
    (Join-Path $sharedDir "cbor_primitives.c")
) -join " "

$includeDirs = "/I `"$coreDir`" /I `"$sharedDir`""
$exePath = Join-Path $outDir "test_agps.exe"

$cmd = "call `"$vcvarsall`" >nul && cl.exe /nologo /W4 /std:c11 $includeDirs /Fe:`"$exePath`" /Fo:`"$outDir\\`" $sources"

Write-Host "Building host agps test..."
cmd.exe /c $cmd
if($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Running host agps test..."
& $exePath
exit $LASTEXITCODE
