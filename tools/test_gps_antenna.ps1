# GPS antenna smoke-test CLI: listens for NMEA sentences for a fixed window and
# prints/logs the best result seen (satellite count, fix quality, HDOP, and
# decimal-degree lat/lon if a real fix was acquired).
#
# By default this does NOT touch the board's flash -- it assumes gps_antenna_test
# is already flashed and just listens. Pass -Flash the first time (or whenever
# esp32/gps_antenna_test/ itself changes) to build and flash it; this REPLACES
# whatever firmware is currently on the board (e.g. the real flipper_esp32_over_ble
# app). Re-flash the real app yourself afterwards if you need it back.
#
# Usage:
#   tools/test_gps_antenna.ps1 -Flash -Label "ceramic-patch-v1" -Seconds 60
#   tools/test_gps_antenna.ps1 -Label "helical-v2" -Seconds 120           # no reflash
#   tools/test_gps_antenna.ps1 -Label "helical-v2" -Seconds 120 -Port COM9
param(
    [string]$Label = "unlabeled",
    [int]$Seconds = 60,
    [string]$Port = "",
    [switch]$Flash
)
$ErrorActionPreference = "Stop"

[Environment]::SetEnvironmentVariable("MSYSTEM", $null)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..")
$projectDir = Join-Path $repoRoot "esp32\gps_antenna_test"
$resultsCsv = Join-Path $scriptDir "gps_antenna_results.csv"
$rawLog = Join-Path $scriptDir "gps_antenna_last_run.log"

function Get-EspPort {
    if ($Port) {
        return $Port
    }
    $devices = Get-PnpDevice -Class Ports -PresentOnly |
        Where-Object { $_.InstanceId -like "USB\VID_303A*" }
    if (-not $devices) {
        throw "No Espressif (VID_303A) serial device found. Plug in the board or pass -Port explicitly."
    }
    $comPorts = @($devices | ForEach-Object {
        if ($_.FriendlyName -match "\((COM\d+)\)") { $Matches[1] }
    })
    if (-not $comPorts) {
        throw "Found an Espressif USB device but couldn't parse its COM port from: $($devices.FriendlyName)"
    }
    if ($comPorts.Count -gt 1) {
        Write-Host "Multiple Espressif serial ports found: $($comPorts -join ', '). Using $($comPorts[0]); pass -Port to override." -ForegroundColor Yellow
    }
    return $comPorts[0]
}

function Convert-NmeaCoord {
    param([string]$Raw, [string]$Hemisphere)
    if ([string]::IsNullOrWhiteSpace($Raw)) { return $null }
    $val = [double]$Raw
    $deg = [math]::Floor($val / 100.0)
    $min = $val - ($deg * 100.0)
    $dec = $deg + ($min / 60.0)
    if ($Hemisphere -eq "S" -or $Hemisphere -eq "W") { $dec = -$dec }
    return [math]::Round($dec, 6)
}

$resolvedPort = Get-EspPort
Write-Host "Using port: $resolvedPort"

. C:\Users\Deyan\esp\esp-idf\export.ps1 | Out-Null

Set-Location $projectDir

if ($Flash) {
    Write-Host "NOTE: building and flashing esp32/gps_antenna_test/ to the board," -ForegroundColor Yellow
    Write-Host "      replacing whatever firmware is currently flashed." -ForegroundColor Yellow
    Write-Host "Building..."
    idf.py -p $resolvedPort build 2>&1 | Select-Object -Last 20
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }

    Write-Host "Flashing..."
    idf.py -p $resolvedPort flash 2>&1 | Select-Object -Last 20
    if ($LASTEXITCODE -ne 0) { throw "Flash failed (exit $LASTEXITCODE)" }
} else {
    Write-Host "Skipping flash, using whatever firmware is currently on the board -- pass -Flash to rebuild/reflash." -ForegroundColor Yellow
}

if (Test-Path $rawLog) { Remove-Item $rawLog -Force }

$env:ESP_IDF_MONITOR_TEST = "1"
$proc = Start-Process -FilePath "powershell.exe" -ArgumentList @(
    "-NoProfile", "-Command",
    "idf.py -p $resolvedPort monitor --timestamps 2>&1 | Out-File -FilePath `"$rawLog`" -Encoding utf8"
) -PassThru -WindowStyle Hidden

Write-Host "Listening for $Seconds seconds..."
Start-Sleep -Seconds $Seconds

Get-CimInstance Win32_Process | Where-Object {
    $_.CommandLine -like "*esp_idf_monitor*" -and $_.CommandLine -like "*$resolvedPort*"
} | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

if (-not (Test-Path $rawLog)) {
    throw "No monitor output captured at $rawLog"
}

$bestSats = 0
$bestFixQuality = 0
$bestHdop = $null
$fixAcquired = $false
$fixLat = $null
$fixLon = $null

Get-Content $rawLog | ForEach-Object {
    if ($_ -match "nmea:\s*(\$[^\r\n]*)") {
        $sentence = $Matches[1].Split("*")[0]
        $fields = $sentence.Split(",")
        if ($fields[0] -match "GGA$") {
            $fixQ = 0
            if ($fields.Count -gt 6 -and $fields[6]) { $fixQ = [int]$fields[6] }
            $sats = 0
            if ($fields.Count -gt 7 -and $fields[7]) { $sats = [int]$fields[7] }
            if ($sats -gt $bestSats) { $bestSats = $sats }
            if ($fixQ -gt $bestFixQuality) {
                $bestFixQuality = $fixQ
                if ($fields.Count -gt 8) { $bestHdop = $fields[8] }
                if ($fixQ -gt 0 -and $fields.Count -gt 5 -and $fields[2] -and $fields[4]) {
                    $fixAcquired = $true
                    $fixLat = Convert-NmeaCoord $fields[2] $fields[3]
                    $fixLon = Convert-NmeaCoord $fields[4] $fields[5]
                }
            }
        }
    }
}

$timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
if ($fixAcquired) {
    $summary = "[$Label] ${Seconds}s listen: FIX ACQUIRED -- lat=$fixLat lon=$fixLon ($bestSats sats, HDOP $bestHdop)"
} else {
    $summary = "[$Label] ${Seconds}s listen: NO FIX (best: $bestSats satellites)"
}
Write-Host ""
Write-Host $summary -ForegroundColor $(if ($fixAcquired) { "Green" } else { "Cyan" })

$row = [PSCustomObject]@{
    Timestamp      = $timestamp
    Label          = $Label
    Seconds        = $Seconds
    BestSatellites = $bestSats
    BestFixQuality = $bestFixQuality
    BestHDOP       = $bestHdop
    FixAcquired    = $fixAcquired
    Latitude       = $fixLat
    Longitude      = $fixLon
    Summary        = $summary
}
$row | Export-Csv -Path $resultsCsv -NoTypeInformation -Append
Write-Host "Result appended to $resultsCsv"
