# Builds, and optionally flashes / boot-log-captures, the ESP32 firmware via ESP-IDF, from a
# clean PowerShell invocation. Clears MSYSTEM (idf.py's export.ps1 refuses to run with it set,
# e.g. from a Git-Bash ancestor shell) and sources export.ps1 itself, so this is the one
# reliable entry point regardless of which shell/tool launched it.
#
#   Build only (default, backward compatible):    .\tools\build_esp32.ps1
#   Build + flash:                                .\tools\build_esp32.ps1 -Port COM9
#   Flash only, skip build:                       .\tools\build_esp32.ps1 -Port COM9 -SkipBuild
#   Build + flash + capture N seconds of boot log: .\tools\build_esp32.ps1 -Port COM9 -CaptureBootLog -CaptureSeconds 10
#
# Reconfirm the port before trusting -Port: COM assignments drift across sessions/reboots
# (see CLAUDE.md). This script only checks the port exists, not that it's the right board.
param(
    [int]$TailLines = 150,
    [string]$Port,
    [switch]$SkipBuild,
    [switch]$CaptureBootLog,
    [int]$CaptureSeconds = 8
)
$ErrorActionPreference = "Stop"

[Environment]::SetEnvironmentVariable("MSYSTEM", $null)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..")
$esp32Dir = Join-Path $repoRoot "esp32"

. C:\Users\Deyan\esp\esp-idf\export.ps1 | Out-Null
Set-Location $esp32Dir

if (-not $SkipBuild) {
    idf.py build 2>&1 | Select-Object -Last $TailLines
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
}

if (-not $Port) {
    if ($SkipBuild) {
        Write-Error "-SkipBuild requires -Port (nothing to do otherwise)"
        exit 1
    }
    exit 0
}

$knownPorts = [System.IO.Ports.SerialPort]::GetPortNames()
if ($knownPorts -notcontains $Port) {
    Write-Error "Port '$Port' not found. Known ports: $($knownPorts -join ', ')"
    exit 1
}

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
    # -ErrorAction Continue overrides this script's global "Stop" for this one call: idf_monitor
    # writes benign notices (e.g. "GDB cannot open serial ports accessed as COMx") to its error
    # stream, and Receive-Job re-emits those as error records -- under "Stop" that silently
    # aborted this whole script before any boot-log output was printed. Continue just prints them
    # alongside the real log instead of treating them as fatal.
    Receive-Job $job -ErrorAction Continue
    Remove-Job $job -Force
}

exit 0
