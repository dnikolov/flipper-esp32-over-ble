# Flashes the built ESP32 firmware to the board via ESP-IDF, with optional post-flash
# boot-log capture. This is the direct flash entry point for the ESP32-C6 target used by
# this project; it does not rebuild unless you pass -BuildFirst.
#
#   .\tools\flash_esp32.ps1 -Port COM9
#   .\tools\flash_esp32.ps1 -Port COM9 -BuildFirst
#   .\tools\flash_esp32.ps1 -Port COM9 -CaptureBootLog -CaptureSeconds 10
#
# Reconfirm the port before trusting -Port: COM assignments drift across sessions/reboots
# (see CLAUDE.md). This script only checks the port exists, not that it's the right board.
param(
    [string]$Port,
    [switch]$BuildFirst,
    [switch]$CaptureBootLog,
    [int]$CaptureSeconds = 8,
    [int]$TailLines = 150
)
$ErrorActionPreference = "Stop"

[Environment]::SetEnvironmentVariable("MSYSTEM", $null)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..")
$esp32Dir = Join-Path $repoRoot "esp32"

. C:\Users\Deyan\esp\esp-idf\export.ps1 | Out-Null
Set-Location $esp32Dir

if (-not $Port) {
    Write-Error "-Port is required (e.g. COM9)"
    exit 1
}

$knownPorts = [System.IO.Ports.SerialPort]::GetPortNames()
if ($knownPorts -notcontains $Port) {
    Write-Error "Port '$Port' not found. Known ports: $($knownPorts -join ', ')"
    exit 1
}

if ($BuildFirst) {
    Write-Host "Building ESP32 firmware before flashing..."
    idf.py build 2>&1 | Select-Object -Last $TailLines
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
}

Write-Host "Flashing ESP32 firmware to $Port..."
idf.py -p $Port flash 2>&1 | Select-Object -Last $TailLines
if ($LASTEXITCODE -ne 0) {
    Write-Error "Flash failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

if ($CaptureBootLog) {
    # ESP_IDF_MONITOR_TEST=1 lets idf.py monitor run without a real interactive TTY (this
    # tool environment has none). Opening the port resets the ESP32-C6 -- expected, not a bug.
    $job = Start-Job -ScriptBlock {
        param($MonitorPort, $WorkDir)
        $env:ESP_IDF_MONITOR_TEST = "1"
        Set-Location $WorkDir
        & idf.py -p $MonitorPort monitor
    } -ArgumentList $Port, $esp32Dir
    Start-Sleep -Seconds $CaptureSeconds
    Stop-Job $job | Out-Null
    Receive-Job $job -ErrorAction Continue
    Remove-Job $job -Force
}

Write-Host "ESP32 flash complete."
exit 0
