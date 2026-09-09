# Builds the ESP32 firmware via ESP-IDF, from a clean PowerShell invocation.
# Clears MSYSTEM (idf.py's export.ps1 refuses to run with it set, e.g. from a Git-Bash
# ancestor shell), sources export.ps1, and runs idf.py build from esp32/.
param(
    [int]$TailLines = 150
)
$ErrorActionPreference = "Stop"

[Environment]::SetEnvironmentVariable("MSYSTEM", $null)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..")
$esp32Dir = Join-Path $repoRoot "esp32"

. C:\Users\Deyan\esp\esp-idf\export.ps1 | Out-Null
Set-Location $esp32Dir
idf.py build 2>&1 | Select-Object -Last $TailLines
exit $LASTEXITCODE
