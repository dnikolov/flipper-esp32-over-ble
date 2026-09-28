# Syncs flipper/ into the pinned Unleashed checkout's applications_user/<AppName> -- this FBT
# revision only resolves APPSRC from a recognized applications_user/ subdirectory on Windows
# (see CLAUDE.md) -- then builds the FAP via fbt.cmd. Reports the built artifact's path and size.
#
#   Build only:               .\tools\build_flipper.ps1
#   Build + transfer to SD:   .\tools\build_flipper.ps1 -Port COM8
#   Debug (-Og) build:        .\tools\build_flipper.ps1 -DebugBuild
#
# Reconfirm the port before trusting -Port: COM assignments drift across sessions/reboots.
#
# Build type (changed 2026-09-28): this builds RELEASE by default (fbt's DEBUG=0, artifacts
# under build\f7-firmware\), not fbt's own default of DEBUG=1 (-Og, build\f7-firmware-D\)
# that this project shipped until then. An external FAP's .text/.rodata/.data/.bss are each
# aligned_malloc()ed from the live Flipper system heap at launch and stay resident for the
# app's whole lifetime (docs/HARDENING_BACKLOG.md H04), so the compiler optimization level is
# a direct runtime-memory decision here, not just a build-speed one: measured on this app,
# -Og -> -Os is .text 65768 -> 54680 and .rodata 12632 -> 11944, i.e. 11799 bytes of system
# heap handed back, with .text -- the single largest contiguous block the ELF loader has to
# find at launch -- down 17%.
#
# Nothing is given up for it: fbt's non-COMPACT release config still defines LOGS_DEBUG_BUILD,
# so every FURI_LOG level still compiles in, and this app contains zero furi_assert() calls
# (it uses furi_check() exclusively, which survives FURI_NDEBUG). Release is also the
# configuration the device's own firmware is built in, so it is the matching one, not a
# riskier one. Pass -DebugBuild to get the old -Og artifact back for a debugger session.
param(
    [string]$UnleashedRoot = "C:\Users\Deyan\unleashed-firmware-unlshd-092",
    [string]$AppName = "flipper_esp32_over_ble",
    [int]$TailLines = 150,
    [string]$Port,
    [switch]$DebugBuild
)
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..")
$flipperSrc = Join-Path $repoRoot "flipper"
$appDest = Join-Path $UnleashedRoot "applications_user\$AppName"

# /MIR mirrors (adds, updates, and removes files at the destination to match the source) so a
# deleted-in-repo file doesn't linger and get built. Exit codes 0-7 are success; 8+ is failure.
robocopy $flipperSrc $appDest /MIR /NFL /NDL /NJH /NJS /NC /NS | Out-Null
if ($LASTEXITCODE -ge 8) {
    Write-Error "robocopy sync from $flipperSrc to $appDest failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

$buildDirName = if ($DebugBuild) { "f7-firmware-D" } else { "f7-firmware" }
Push-Location $UnleashedRoot
try {
    # Under "Stop", PS 5.1 turns the first native stderr line merged via 2>&1 into a
    # terminating NativeCommandError; scope "Continue" around the call and rely on $LASTEXITCODE.
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        if ($DebugBuild) {
            & .\fbt.cmd "fap_$AppName" 2>&1 | ForEach-Object { "$_" } | Select-Object -Last $TailLines
        } else {
            & .\fbt.cmd DEBUG=0 "fap_$AppName" 2>&1 | ForEach-Object { "$_" } | Select-Object -Last $TailLines
        }
        $buildExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedEap
    }
} finally {
    Pop-Location
}
if ($buildExit -ne 0) {
    Write-Error "fbt build failed (exit $buildExit)"
    exit $buildExit
}

$artifact = Join-Path $UnleashedRoot "build\$buildDirName\.extapps\$AppName.fap"
if (-not (Test-Path $artifact)) {
    Write-Error "Expected artifact not found at $artifact"
    exit 1
}
$size = (Get-Item $artifact).Length
Write-Output "Artifact: $artifact ($size bytes)"

if (-not $Port) {
    exit 0
}

& (Join-Path $scriptDir "flash_flipper.ps1") -UnleashedRoot $UnleashedRoot -AppName $AppName -FapPath $artifact -Port $Port
exit $LASTEXITCODE
