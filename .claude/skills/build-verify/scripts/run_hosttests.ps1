# Runs every host-native codec/pairing/session/GPS/protocol test build script under tests/esp32
# and tests/flipper (each already builds with cl.exe and runs its own executable, exiting
# non-zero on any assertion failure). No single existing script covers all of them, so the
# build-verify skill's `hosttests` target uses this wrapper instead of an inline command list.
# Reports one PASS/FAIL line per script, plus the last ~$TailLines lines of output for any
# script that failed.
param(
    [int]$TailLines = 15
)
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..\..\..\..")

# vcvars64.bat's own internal tool-chain resolution shells out to a bare "vswhere.exe" (not a
# full path), which fails with a bare "not recognized" error in any PowerShell session whose
# PATH doesn't already include the VS Installer directory -- observed in a non-interactive tool
# session that otherwise has cl.exe/vswhere.exe available only by full path. Prepend it
# defensively; a no-op if it's already on PATH or the directory doesn't exist.
foreach ($pf in @("${env:ProgramFiles(x86)}", "${env:ProgramFiles}")) {
    $vsInstallerDir = Join-Path $pf "Microsoft Visual Studio\Installer"
    if ((Test-Path $vsInstallerDir) -and ($env:PATH -notlike "*$vsInstallerDir*")) {
        $env:PATH = "$env:PATH;$vsInstallerDir"
    }
}

$scripts = @(
    "tests\esp32\build.ps1",
    "tests\esp32\build_pairing.ps1",
    "tests\esp32\build_session.ps1",
    "tests\esp32\build_location.ps1",
    "tests\esp32\build_wardriving.ps1",
    "tests\esp32\build_cluster_link.ps1",
    "tests\esp32\build_meshcore_proto.ps1",
    "tests\esp32\build_meshtastic_proto.ps1",
    "tests\flipper\build.ps1",
    "tests\flipper\build_pairing.ps1",
    "tests\flipper\build_session.ps1"
)

$anyFail = $false
foreach ($rel in $scripts) {
    $path = Join-Path $repoRoot $rel
    if (-not (Test-Path $path)) {
        Write-Output "SKIP: $rel (not found)"
        continue
    }

    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $output = & $path 2>&1 | ForEach-Object { "$_" }
    $exit = $LASTEXITCODE
    $ErrorActionPreference = $savedEap

    if ($exit -eq 0) {
        Write-Output "PASS: $rel"
    } else {
        $anyFail = $true
        Write-Output "FAIL: $rel (exit $exit)"
        $output | Select-Object -Last $TailLines | ForEach-Object { Write-Output "    $_" }
    }
}

if ($anyFail) { exit 1 } else { exit 0 }
