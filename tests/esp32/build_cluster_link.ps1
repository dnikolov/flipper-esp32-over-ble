# Builds and runs the host-native test for components/feb_cluster_link (docs/CLUSTER.md's
# "Inter-board protocol" frame spec). Mirrors build.ps1's vswhere/vcvars64/cl.exe setup for
# the existing feb_protocol codec tests -- see that script for the pattern being followed.
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
$sharedDir = Join-Path $repoRoot "components\feb_cluster_link"
$outDir = Join-Path $scriptDir "build"

if(-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir | Out-Null
}

$sources = @(
    (Join-Path $scriptDir "test_cluster_link.c"),
    (Join-Path $sharedDir "crc16.c"),
    (Join-Path $sharedDir "cluster_link.c")
) -join " "

$includeDirs = "/I `"$sharedDir`""
$exePath = Join-Path $outDir "test_cluster_link.exe"

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
