<#
Step 4 (docs/PLAN.md) radio-coexistence sweep orchestrator.

Drives the throwaway ESP32 coex_test harness through its full sweep unattended:
launches the Flipper's FAP over its CLI (COM8), flashes/monitors the ESP32 (COM9),
detects a silent hang (no COEX_ log line for HangThresholdSec) and recovers by
reflashing (a real esptool hardware reset, not a soft reset - the harness resumes
from NVS-persisted point_index/attempt_count on the next boot), and stops at
WallClockCeilingHours regardless of sweep progress. All raw serial output and
parsed COEX_* events are written under tools/coex/logs/<run-id>/ for later review;
this script does not compute recommended interval bounds - that judgment call is
made from the collected numbers after the run, per docs/PLAN.md step 4.

Usage: powershell -File run_coex_sweep.ps1 [-DryRunFlipperOnly]
#>

param(
    [string]$EspPort = "COM9",
    [string]$FlipperPort = "COM8",
    [string]$EspIdfExport = "C:\Users\Deyan\esp\esp-idf\export.ps1",
    [string]$ProjectDir = "c:\Users\Deyan\flipper-esp32-over-ble\esp32\coex_test",
    [string]$FapSdPath = "/ext/apps/Connectivity/flipper_esp32_over_ble.fap",
    [int]$HangThresholdSec = 180,
    [double]$WallClockCeilingHours = 6.0,
    [switch]$SkipFlipperLaunch,
    [switch]$SkipFlipperHealthCheck,
    [switch]$SkipInitialFlash
)

$ErrorActionPreference = "Stop"

$RunId = Get-Date -Format "yyyyMMdd_HHmmss"
$LogDir = Join-Path $PSScriptRoot "logs\$RunId"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$RawLogPath = Join-Path $LogDir "serial_raw.log"
$EventsLogPath = Join-Path $LogDir "events.jsonl"
$RunLogPath = Join-Path $LogDir "orchestrator.log"

function Write-RunLog {
    param([string]$Message)
    $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message
    Write-Host $line
    try { Add-Content -Path $RunLogPath -Value $line -ErrorAction Stop } catch { Write-Host "WARNING: could not write orchestrator log (non-fatal): $($_.Exception.Message)" }
}

function Send-FlipperCli {
    param([System.IO.Ports.SerialPort]$Port, [string]$Command, [int]$SettleMs = 400)
    $Port.WriteLine($Command)
    Start-Sleep -Milliseconds $SettleMs
}

function Start-FlipperApp {
    param([string]$PortName, [string]$FapPath)
    Write-RunLog "Opening Flipper CLI on $PortName to launch $FapPath"
    $fp = New-Object System.IO.Ports.SerialPort $PortName, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
    $fp.NewLine = "`r`n"
    $fp.ReadTimeout = 2000
    $fp.WriteTimeout = 2000
    $fp.Open()
    try {
        Start-Sleep -Milliseconds 300
        Send-FlipperCli -Port $fp -Command ""
        Send-FlipperCli -Port $fp -Command "loader open `"$FapPath`""
        Send-FlipperCli -Port $fp -Command "input send ok press"
        Send-FlipperCli -Port $fp -Command "input send ok short"
        Send-FlipperCli -Port $fp -Command "input send ok release"
        Start-Sleep -Milliseconds 500
        Send-FlipperCli -Port $fp -Command "loader info"
        Start-Sleep -Milliseconds 300
        $resp = ""
        try { while ($true) { $resp += $fp.ReadExisting(); Start-Sleep -Milliseconds 100; if ($fp.BytesToRead -eq 0) { break } } } catch {}
        Add-Content -Path (Join-Path $LogDir "flipper_launch.log") -Value $resp
        Write-RunLog "Flipper launch sequence sent; response captured in flipper_launch.log"
    }
    finally {
        $fp.Close()
    }
}

function Test-FlipperAlive {
    param([string]$PortName, [string]$FapPath)
    $fp = New-Object System.IO.Ports.SerialPort $PortName, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
    $fp.NewLine = "`r`n"
    $fp.ReadTimeout = 2000
    $fp.WriteTimeout = 2000
    try {
        $fp.Open()
        Send-FlipperCli -Port $fp -Command "loader info" -SettleMs 500
        $resp = $fp.ReadExisting()
        Add-Content -Path (Join-Path $LogDir "flipper_health.log") -Value ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $resp)
        return $resp -match [Regex]::Escape($FapPath) -or $resp -match "flipper_esp32_over_ble"
    }
    catch {
        Add-Content -Path (Join-Path $LogDir "flipper_health.log") -Value ("[{0}] health check failed: {1}" -f (Get-Date -Format "HH:mm:ss"), $_.Exception.Message)
        return $false
    }
    finally {
        if ($fp.IsOpen) { $fp.Close() }
    }
}

function Invoke-EspFlash {
    param([string]$ProjectDir, [string]$Port, [string]$IdfExport)
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
    $outLog = Join-Path $LogDir "flash_${stamp}_out.log"
    $errLog = Join-Path $LogDir "flash_${stamp}_err.log"
    $cmd = "Remove-Item Env:\MSYSTEM -ErrorAction SilentlyContinue; & '$IdfExport' | Out-Null; Set-Location '$ProjectDir'; idf.py -p $Port flash; exit `$LASTEXITCODE"
    Write-RunLog "Flashing $Port from $ProjectDir ..."
    $proc = Start-Process -FilePath "powershell.exe" `
        -ArgumentList @("-NoProfile", "-NonInteractive", "-Command", $cmd) `
        -PassThru -Wait -WindowStyle Hidden `
        -RedirectStandardOutput $outLog -RedirectStandardError $errLog
    Write-RunLog "Flash exit code: $($proc.ExitCode) (see $outLog / $errLog)"
    return $proc.ExitCode
}

function Add-Event {
    param([hashtable]$Fields)
    $Fields["_ts"] = (Get-Date).ToString("o")
    try {
        ($Fields | ConvertTo-Json -Compress) | Add-Content -Path $EventsLogPath -ErrorAction Stop
    } catch {
        Write-Host "WARNING: could not write event log (non-fatal): $($_.Exception.Message)"
    }
}

function Parse-CoexLine {
    param([string]$Line)
    # Case-sensitive match is required: the ESP_LOGI tag "coex_test:" (lowercase)
    # prefixes every line from this module and would otherwise satisfy a
    # case-insensitive "COEX_" match before the real uppercase COEX_* payload is reached.
    if ($Line -cnotmatch "COEX_([A-Z_]+)(.*)") { return $null }
    $eventName = $Matches[1]
    $fields = @{ event = "COEX_$eventName" }
    foreach ($kv in [Regex]::Matches($Matches[2], '(\w+)=(\S+)')) {
        $fields[$kv.Groups[1].Value] = $kv.Groups[2].Value
    }
    return $fields
}

Write-RunLog "=== Step 4 coexistence sweep starting. Run ID: $RunId ==="
Write-RunLog "ESP32 port: $EspPort, Flipper port: $FlipperPort, project: $ProjectDir"
Write-RunLog "Hang threshold: ${HangThresholdSec}s, wall-clock ceiling: ${WallClockCeilingHours}h"

if (-not (Test-Path $ProjectDir)) {
    Write-RunLog "FATAL: project dir $ProjectDir does not exist. Aborting."
    exit 1
}

if (-not $SkipFlipperLaunch) {
    Start-FlipperApp -PortName $FlipperPort -FapPath $FapSdPath
} else {
    Write-RunLog "Skipping Flipper launch (assumed already running) per -SkipFlipperLaunch."
}

if (-not $SkipInitialFlash) {
    $flashExit = Invoke-EspFlash -ProjectDir $ProjectDir -Port $EspPort -IdfExport $EspIdfExport
    if ($flashExit -ne 0) {
        Write-RunLog "FATAL: initial flash failed (exit $flashExit). Aborting before starting the sweep."
        exit 1
    }
} else {
    Write-RunLog "Skipping initial flash per -SkipInitialFlash (board assumed already flashed and running)."
}

$sweepStart = Get-Date
$ceiling = $sweepStart.AddHours($WallClockCeilingHours)
$lastLineTime = Get-Date
$reflashCount = 0
$lastFlipperHealthCheck = Get-Date
$pointResults = New-Object System.Collections.Generic.List[hashtable]
$sweepDone = $false

$serial = $null
function Open-EspSerial {
    param([string]$PortName)
    $sp = New-Object System.IO.Ports.SerialPort $PortName, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
    $sp.ReadTimeout = 5000
    $sp.Open()
    return $sp
}

try {
    $serial = Open-EspSerial -PortName $EspPort
    Write-RunLog "Serial monitor open on $EspPort. Watching for COEX_* events."

    while (-not $sweepDone) {
        $now = Get-Date
        if ($now -gt $ceiling) {
            Write-RunLog "Wall-clock ceiling (${WallClockCeilingHours}h) reached. Stopping sweep monitoring; results so far are final for this run."
            break
        }

        if ((-not $SkipFlipperHealthCheck) -and ($now - $lastFlipperHealthCheck).TotalMinutes -ge 5) {
            $lastFlipperHealthCheck = $now
            if ($serial.IsOpen) { $serial.Close() }
            $alive = Test-FlipperAlive -PortName $FlipperPort -FapPath $FapSdPath
            Write-RunLog "Flipper health check: app running = $alive"
            if (-not $alive) {
                Write-RunLog "Flipper app not detected as running - relaunching (does not affect ESP32-side sweep state)."
                Start-FlipperApp -PortName $FlipperPort -FapPath $FapSdPath
            }
            $serial = Open-EspSerial -PortName $EspPort
        }

        $line = $null
        try {
            $line = $serial.ReadLine()
        }
        catch [System.TimeoutException] {
            $line = $null
        }
        catch [System.InvalidOperationException] {
            Write-RunLog "Serial port not open - reopening."
            Start-Sleep -Seconds 2
            $serial = Open-EspSerial -PortName $EspPort
            continue
        }

        if ($null -ne $line) {
            $line = $line.TrimEnd()
            $lastLineTime = Get-Date
            try { Add-Content -Path $RawLogPath -Value $line -ErrorAction Stop } catch { Write-Host "WARNING: could not write raw serial log (non-fatal): $($_.Exception.Message)" }

            $parsed = Parse-CoexLine -Line $line
            if ($null -ne $parsed) {
                Add-Event -Fields $parsed
                switch ($parsed.event) {
                    "COEX_POINT_RESULT" { $pointResults.Add($parsed); Write-RunLog "POINT RESULT: $line" }
                    "COEX_POINT_UNSTABLE" { $pointResults.Add($parsed); Write-RunLog "POINT UNSTABLE: $line" }
                    "COEX_SWEEP_DONE" { $sweepDone = $true; Write-RunLog "SWEEP DONE: $line" }
                    "COEX_BOOT" { Write-RunLog "Board booted: $line" }
                    default { }
                }
            }
            continue
        }

        $gapSec = ((Get-Date) - $lastLineTime).TotalSeconds
        if ($gapSec -ge $HangThresholdSec) {
            $reflashCount++
            Write-RunLog "HANG DETECTED: no serial output for ${gapSec}s (threshold ${HangThresholdSec}s). Reflash-recovery #$reflashCount."
            Add-Event -Fields @{ event = "ORCH_HANG_RECOVERY"; gap_sec = [math]::Round($gapSec, 1); reflash_count = $reflashCount }
            if ($serial.IsOpen) { $serial.Close() }
            $flashExit = Invoke-EspFlash -ProjectDir $ProjectDir -Port $EspPort -IdfExport $EspIdfExport
            if ($flashExit -ne 0) {
                Write-RunLog "Reflash-recovery attempt failed (exit $flashExit). Waiting 30s and retrying reflash once more."
                Start-Sleep -Seconds 30
                $flashExit = Invoke-EspFlash -ProjectDir $ProjectDir -Port $EspPort -IdfExport $EspIdfExport
                if ($flashExit -ne 0) {
                    Write-RunLog "FATAL: reflash-recovery failed twice in a row. Stopping orchestrator; board may need manual attention."
                    break
                }
            }
            $serial = Open-EspSerial -PortName $EspPort
            $lastLineTime = Get-Date
        }
    }
}
finally {
    if ($null -ne $serial -and $serial.IsOpen) { $serial.Close() }
}

$elapsed = (Get-Date) - $sweepStart
Write-RunLog "=== Sweep monitoring ended. Elapsed: $($elapsed.ToString('hh\:mm\:ss')). Sweep done: $sweepDone. Reflash-recoveries: $reflashCount ==="

$reportPath = Join-Path $LogDir "report.md"
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("# Step 4 coexistence sweep - run $RunId")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("- Sweep completed cleanly: **$sweepDone**")
[void]$sb.AppendLine("- Elapsed wall-clock: $($elapsed.ToString('hh\:mm\:ss')) (ceiling: ${WallClockCeilingHours}h)")
[void]$sb.AppendLine("- Orchestrator-triggered reflash-recoveries (silent hangs): $reflashCount")
[void]$sb.AppendLine("- Raw serial log: ``$RawLogPath``")
[void]$sb.AppendLine("- Parsed event stream: ``$EventsLogPath``")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("## Per-point results")
[void]$sb.AppendLine("")
foreach ($r in $pointResults) {
    [void]$sb.AppendLine("- " + (($r.GetEnumerator() | Where-Object { $_.Key -ne "_ts" -and $_.Key -ne "event" } | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join " "))
}
[void]$sb.AppendLine("")
[void]$sb.AppendLine("No interval bounds are recommended by this script - review the numbers above against docs/PLAN.md step 4's done-when criteria and record chosen safe min/max/default bounds in docs/PLAN.md / docs/SESSION_MEMORY.md by hand.")
Set-Content -Path $reportPath -Value $sb.ToString()
Write-RunLog "Report written to $reportPath"
