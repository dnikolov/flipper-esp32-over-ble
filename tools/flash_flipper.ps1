# Transfers a built Flipper FAP onto the device's SD card via the pinned Unleashed checkout's
# scripts/runfap.py -- the actual working transfer method (the Flipper's SD card does not mount
# as a USB mass-storage drive, and runfap.py speaks the Flipper CLI's storage protocol over its
# serial port instead). Never auto-launches the app afterward: a prior auto-launch attempt via
# `fbt.cmd launch APPSRC=...` hit a transient "not enough memory" preload error -- restart the
# Flipper and launch the app manually instead.
#
#   .\tools\flash_flipper.ps1 -Port COM8
#   .\tools\flash_flipper.ps1 -Port COM8 -FapPath C:\path\to\some.fap
#
# Reconfirm the port before trusting -Port: COM assignments drift across sessions/reboots.
param(
    [string]$UnleashedRoot = "C:\Users\Deyan\unleashed-firmware-unlshd-092",
    [string]$AppName = "flipper_esp32_over_ble",
    [string]$FapPath,
    [string]$Port = "auto",
    [string]$Target
)
$ErrorActionPreference = "Stop"

if (-not $FapPath) {
    $FapPath = Join-Path $UnleashedRoot "build\f7-firmware-D\.extapps\$AppName.fap"
}
if (-not (Test-Path $FapPath)) {
    Write-Error "FAP not found at $FapPath -- build it first (tools/build_flipper.ps1)"
    exit 1
}
if (-not $Target) {
    $Target = "/ext/apps/Connectivity/$AppName.fap"
}

if ($Port -ne "auto") {
    $knownPorts = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($knownPorts -notcontains $Port) {
        Write-Error "Port '$Port' not found. Known ports: $($knownPorts -join ', ')"
        exit 1
    }
}

Push-Location $UnleashedRoot
try {
    python scripts/runfap.py -p $Port -s $FapPath -t $Target
    $transferExit = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($transferExit -ne 0) {
    Write-Error "runfap.py transfer failed (exit $transferExit)"
    exit $transferExit
}

Write-Output "Transferred $FapPath -> $Target on the Flipper's SD card."
Write-Output "Not auto-launched -- restart the Flipper and launch the app manually."
