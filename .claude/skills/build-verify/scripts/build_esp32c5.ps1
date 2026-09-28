# Build-only wrapper for the ESP32-C5 firmware, for the build-verify skill's `esp32c5` target.
# No tools/*.ps1 wrapper is committed for this board yet (esp32c5-developer.md's own build
# snippet is bare `idf.py build`); this mirrors tools/build_esp32.ps1's Invoke-Native/MSYSTEM
# pattern rather than inventing an inline escaped command.
param(
    [int]$TailLines = 150
)
$ErrorActionPreference = "Stop"

[Environment]::SetEnvironmentVariable("MSYSTEM", $null)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..\..\..\..")
$boardDir = Join-Path $repoRoot "esp32c5"

. C:\Users\Deyan\esp\esp-idf\export.ps1 | Out-Null
Set-Location $boardDir

# Under "Stop", PS 5.1 turns the first native stderr line merged via 2>&1 into a terminating
# NativeCommandError (idf.py/cmake/ninja write warnings there); run native calls under
# "Continue" and rely on the exit code instead.
function Invoke-Native([scriptblock]$Block) {
    $ErrorActionPreference = "Continue"
    & $Block 2>&1 | ForEach-Object { "$_" } | Select-Object -Last $TailLines
    return
}

Invoke-Native { idf.py build }
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}
exit 0
