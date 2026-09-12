# Transfers a built Flipper FAP onto the device's SD card via the pinned Unleashed checkout's
# scripts/runfap.py -- the actual working transfer method (the Flipper's SD card does not mount
# as a USB mass-storage drive, and runfap.py speaks the Flipper CLI's storage protocol over its
# serial port instead). This wrapper never asks for a launch, but runfap.py itself unconditionally
# sends a `loader open` after every transfer to launch the app, with no flag to suppress it -- and
# that launch reliably fails with a transient "not enough memory" preload error on this device
# (confirmed repeatedly, most recently 2026-09-12), which surfaces as a non-zero exit from this
# script even though the file transfer itself succeeded. Restart the Flipper and launch the app
# manually -- do not treat this script's exit code alone as proof the transfer failed; check its
# output for "Transferred ... on the Flipper's SD card" first.
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
