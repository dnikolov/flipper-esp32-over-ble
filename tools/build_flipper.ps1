# Syncs flipper/ into the pinned Unleashed checkout's applications_user/<AppName> -- this FBT
# revision only resolves APPSRC from a recognized applications_user/ subdirectory on Windows
# (see CLAUDE.md) -- then builds the FAP via fbt.cmd. Reports the built artifact's path and size.
#
#   Build only:               .\tools\build_flipper.ps1
#   Build + transfer to SD:   .\tools\build_flipper.ps1 -Port COM8
#
# Reconfirm the port before trusting -Port: COM assignments drift across sessions/reboots.
param(
    [string]$UnleashedRoot = "C:\Users\Deyan\unleashed-firmware-unlshd-092",
    [string]$AppName = "flipper_esp32_over_ble",
    [int]$TailLines = 150,
    [string]$Port
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

Push-Location $UnleashedRoot
try {
    & .\fbt.cmd "fap_$AppName" 2>&1 | Select-Object -Last $TailLines
    $buildExit = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($buildExit -ne 0) {
    Write-Error "fbt build failed (exit $buildExit)"
    exit $buildExit
}

$artifact = Join-Path $UnleashedRoot "build\f7-firmware-D\.extapps\$AppName.fap"
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
