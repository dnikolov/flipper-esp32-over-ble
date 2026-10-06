# Publishes the Flipper's current wardriving CSV to wdgwars.pl.
#
# Fetched and run unattended via a BadUSB-triggered bootstrap (see docs/WARDRIVING_PUBLISH.md).
# Speaks the Flipper's built-in `storage` CLI command directly over the CDC-serial port -- no
# Python/scripts/storage.py dependency, so it runs on any Windows machine with just PowerShell.
# Protocol reference: docs/references/flipper-firmware/upstream/scripts/flipper/storage.py and
# applications/services/storage/storage_cli.c.

[CmdletBinding()]
param(
    [string]$Port = "auto",
    [string]$AppDataPath = "/ext/apps_data/flipper_esp32_over_ble",
    [string]$UploadUrl = "https://wdgwars.pl/api/upload-csv",
    [string]$MeshUploadUrl = "https://wdgwars.pl/api/upload",
    # Whole-run time budget. The FAP gives up waiting for the result file after 600 s (HP-10),
    # measured from before BadUSB even types the bootstrap, so this script must be done -- or
    # have deliberately skipped archiving -- well before that. Archiving is skipped (never a
    # delete) once the remaining budget can't cover it.
    [int]$BudgetSec = 450,
    # Publish: wdgwars upload only (default; what older pinned FAPs launch). Agps: set the
    # Flipper RTC + push agps.bin, never touches the wdgwars key. All: Publish, then Agps.
    [ValidateSet("All", "Publish", "Agps")]
    [string]$Mode = "Publish",
    # Local agps_gen.exe for development; skips the download AND the SHA-256 check.
    [string]$AgpsGenPath = $env:FEB_AGPS_GEN_PATH
)

$ErrorActionPreference = "Stop"

# Release-time constants (docs/AGPS.md "Build contract"). Fill $AgpsGenTag and $AgpsGenSha256
# from the published agps_gen.exe release asset; the all-zero hash makes Agps mode refuse to run.
$AgpsGenTag = "agps-gen-v0"
$AgpsGenSha256 = "0000000000000000000000000000000000000000000000000000000000000000"
$AgpsGenUrl = "https://github.com/dnikolov/flipper-esp32-over-ble/releases/download/$AgpsGenTag/agps_gen.exe"

$script:RunClock = [System.Diagnostics.Stopwatch]::StartNew()
# Held back from every budgeted step for the final CLI resync + result-file write.
$script:ResultReserveSec = 30
# Conservative serial write-back rate used to decide whether the copy-based archive fits.
$script:ArchiveMinBytesPerSec = 20480
# Assumed (not measured) on-device `storage rename` rate: storage_common_rename() is a
# storage_common_copy() in 512 B SD reads/writes plus a delete, no USB round trips. Only used to
# size the rename fallback's CLI timeout.
$script:RenameMinBytesPerSec = 102400
# The FAP reads at most 512 B of the result file (FEB_PUBLISH_RESULT_MAX_LEN) into a 96 B
# message buffer.
$script:ResultMaxBytes = 500
$script:ResultMessageMaxChars = 90

$script:CliPrompt = ">: "
$script:CliEol = "`r`n"
# The wardriving CSV (current + archived) lives in its own "wardriving" subdirectory
# alongside the FAP's older per-calendar-day export files; the credentials and result files
# are publish-flow plumbing, not wardriving data, and stay flat at the app data root.
$script:WardrivingDir = "$AppDataPath/wardriving"
$script:CsvFlipperPath = "$script:WardrivingDir/wardriving_current.csv"
$script:CredentialsFlipperPath = "$AppDataPath/wdgwars_credentials.txt"
$script:ResultFlipperPath = "$AppDataPath/wardriving_publish_result.txt"
# mesh_log's own accumulator (docs/WARDRIVING_PUBLISH.md "Flipper-side storage") -- own
# subdirectory, own independent current/archive lifecycle, distinct from wardriving/'s CSV.
$script:AgpsFlipperPath = "$AppDataPath/agps.bin"
$script:AgpsTmpFlipperPath = "$AppDataPath/agps.bin.tmp"
$script:AgpsResultFlipperPath = "$AppDataPath/agps_sync_result.txt"
$script:AgpsDays = 7
$script:AgpsMaxBytes = 200 * 1024
$script:AgpsGenTimeoutSec = 600
$script:AgpsClockToleranceSec = 5
$script:MeshDir = "$AppDataPath/mesh"
$script:MeshNodesFlipperPath = "$script:MeshDir/mesh_nodes_current.txt"

# Flipper Zero CDC is always USB VID 0483 (STMicro). The IDs below are the USB-serial bridges
# of this project's ESP32 boards (C6 native USB-Serial-JTAG, Heltec CP210x, CH34x) and
# Bluetooth virtual COM ports: opening those with DTR resets the board (wiping live Heltec
# state) or blocks Open() for seconds, so they are never probed.
$script:FlipperUsbVid = "VID_0483"
$script:NeverProbePnpIds = @("VID_303A", "VID_10C4", "VID_1A86", "BTHENUM")

function Get-RemainingSec { return $BudgetSec - $script:RunClock.Elapsed.TotalSeconds }
function Get-SpendableSec { return (Get-RemainingSec) - $script:ResultReserveSec }

# ---------------------------------------------------------------------------
# Low-level CLI-over-serial client
# ---------------------------------------------------------------------------

# COMn -> PNPDeviceID for every present serial device, or $null if Windows device info can't
# be queried at all.
function Get-SerialPortPnpMap {
    $entities = $null
    try {
        $entities = @(Get-CimInstance -ClassName Win32_PnPEntity -Filter "Name LIKE '%(COM%'" -ErrorAction Stop)
    } catch {
        try {
            $entities = @(Get-WmiObject -Class Win32_PnPEntity -Filter "Name LIKE '%(COM%'" -ErrorAction Stop)
        } catch {
            return $null
        }
    }
    $map = @{}
    foreach ($entity in $entities) {
        if ([string]$entity.Name -match '\((COM\d+)\)') { $map[$matches[1]] = [string]$entity.PNPDeviceID }
    }
    return $map
}

function Test-NeverProbePort {
    param([string]$PnpId)
    if (-not $PnpId) { return $false }
    $upper = $PnpId.ToUpperInvariant()
    foreach ($id in $script:NeverProbePnpIds) {
        if ($upper.Contains($id)) { return $true }
    }
    return $false
}

function Test-FlipperPortProbe {
    param([string]$Name)
    $probe = $null
    try {
        $probe = New-Object System.IO.Ports.SerialPort $Name, 115200
        $probe.ReadTimeout = 1500
        $probe.WriteTimeout = 1500
        # The Flipper's CLI-over-VCP only starts its shell (and prints a prompt) once
        # DTR goes active (applications/services/cli/cli_vcp.c: "'Connected' events
        # are generated by DTR going active") -- .NET's SerialPort defaults DtrEnable
        # to false, unlike pyserial (which is why scripts/storage.py never needed
        # this). Without this, the port opens fine but the Flipper never sees a
        # connection and never responds, no matter how long this probe waits.
        $probe.DtrEnable = $true
        $probe.Open()
        Start-Sleep -Milliseconds 300
        $buffer = New-Object System.Text.StringBuilder
        $probeDeadline = (Get-Date).AddSeconds(2)
        while ((Get-Date) -lt $probeDeadline) {
            try {
                $b = $probe.ReadByte()
                if ($b -ge 0) { [void]$buffer.Append([char]$b) }
                if ($buffer.ToString().Contains($script:CliPrompt)) { return $true }
            } catch [TimeoutException] { break }
        }
        return $false
    } catch {
        return $false
    } finally {
        if ($probe -and $probe.IsOpen) { $probe.Close() }
    }
}

# Retries the whole port scan, not just a single port open, for up to $TimeoutSec --
# immediately after BadUSB types the bootstrap, the Flipper is still switching its USB
# personality back from HID to its normal CDC/serial mode, and Windows can take several
# seconds to detach the HID device, load the CDC driver, and assign a COM port. A single
# GetPortNames() snapshot taken right as the script starts can easily miss it entirely, since
# the port doesn't exist yet at that instant, not just fail to open (real-world failure seen
# 2026-09-18: the script started and failed in well under a second, before the Flipper had
# finished re-enumerating).
#
# Only ports whose PnP ID carries the Flipper's USB VID are probed. If none shows up within
# $FallbackAfterSec, every other port is probed except known ESP32 bridges / Bluetooth ports.
function Find-FlipperPort {
    param([int]$TimeoutSec = 20, [int]$FallbackAfterSec = 10)

    $start = Get-Date
    $deadline = $start.AddSeconds($TimeoutSec)
    $printedWaitMessage = $false
    $printedFallbackMessage = $false
    $printedNoPnpMessage = $false
    while ((Get-Date) -lt $deadline) {
        $present = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object -Unique)
        $pnp = Get-SerialPortPnpMap
        $candidates = @()
        if ($null -ne $pnp) {
            $candidates = @($present | Where-Object {
                $pnp.ContainsKey($_) -and $pnp[$_].ToUpperInvariant().Contains($script:FlipperUsbVid)
            })
            if ($candidates.Count -eq 0 -and ((Get-Date) - $start).TotalSeconds -ge $FallbackAfterSec) {
                $candidates = @($present | Where-Object { -not (Test-NeverProbePort -PnpId $pnp[$_]) })
                if (-not $printedFallbackMessage -and $candidates.Count -gt 0) {
                    Write-Host "No USB VID 0483 serial device found -- probing other ports ($($candidates -join ', ')), skipping ESP32 bridges..."
                    $printedFallbackMessage = $true
                }
            }
        } else {
            if (-not $printedNoPnpMessage) {
                Write-Host "Could not query Windows device info -- probing every serial port."
                $printedNoPnpMessage = $true
            }
            $candidates = $present
        }
        foreach ($name in $candidates) {
            if (Test-FlipperPortProbe -Name $name) { return $name }
        }
        if (-not $printedWaitMessage) {
            Write-Host "Flipper not found yet -- waiting for it to finish switching back from BadUSB mode..."
            $printedWaitMessage = $true
        }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

function Open-FlipperCli {
    param([string]$PortName)

    $serial = New-Object System.IO.Ports.SerialPort $PortName, 115200
    $serial.ReadTimeout = 10000
    $serial.WriteTimeout = 60000
    # See Test-FlipperPortProbe's own comment -- DTR must be asserted for the Flipper's CLI
    # shell to start at all.
    $serial.DtrEnable = $true
    $serial.Open()
    Start-Sleep -Milliseconds 500

    $buffer = New-Object System.Collections.Generic.List[byte]
    Read-FlipperUntil -Serial $serial -Buffer ([ref]$buffer) -Terminator $script:CliPrompt -TimeoutSec 10 | Out-Null

    $cli = @{ Serial = $serial; Buffer = $buffer; PortName = $PortName }
    Sync-FlipperCli -Cli $cli -TimeoutSec 10
    return $cli
}

# Flushes stray input with a known-syntax command whose output carries a unique marker,
# matching storage.py's own startup sync. Because the CLI processes input strictly in order,
# this also realigns reads after a timed-out command: any late output from it arrives before
# the marker and is skipped.
function Sync-FlipperCli {
    param($Cli, [int]$TimeoutSec = 10)
    $Cli.Buffer.Clear()
    $Cli.Serial.DiscardInBuffer()
    Send-FlipperLine -Serial $Cli.Serial -Line "device_info`r"
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator "hardware_model" -TimeoutSec $TimeoutSec | Out-Null
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt -TimeoutSec $TimeoutSec | Out-Null
}

# After any CLI timeout: resync in place, or, if the CLI is stuck mid-command (e.g. a
# write_chunk still waiting for raw bytes), drop DTR to end that session and reopen.
function Restore-FlipperCli {
    param($Cli)
    $timeout = [int][Math]::Max(5, [Math]::Min(30, (Get-RemainingSec) - 10))
    try {
        Sync-FlipperCli -Cli $Cli -TimeoutSec $timeout
        return
    } catch {
        Write-Host "CLI resync failed ($($_.Exception.Message)) -- reopening the port..."
    }
    try { if ($Cli.Serial.IsOpen) { $Cli.Serial.Close() } } catch { }
    Start-Sleep -Milliseconds 500
    $fresh = Open-FlipperCli -PortName $Cli.PortName
    $Cli.Serial = $fresh.Serial
    $Cli.Buffer = $fresh.Buffer
}

function Close-FlipperCli {
    param($Cli)
    if ($Cli.Serial.IsOpen) { $Cli.Serial.Close() }
}

# Reads from the port (draining any bytes already buffered from a previous call first) until
# $Terminator appears, returning everything before it. Mirrors storage.py's BufferedRead.until().
function Read-FlipperUntil {
    param($Serial, [ref]$Buffer, [string]$Terminator, [int]$TimeoutSec = 10)

    $termBytes = [System.Text.Encoding]::ASCII.GetBytes($Terminator)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)

    while ($true) {
        $idx = Find-ByteSequence -Haystack $Buffer.Value -Needle $termBytes
        if ($idx -ge 0) {
            $result = $Buffer.Value.GetRange(0, $idx).ToArray()
            $Buffer.Value.RemoveRange(0, $idx + $termBytes.Length)
            return [System.Text.Encoding]::ASCII.GetString($result)
        }

        if ((Get-Date) -gt $deadline) {
            throw "Timed out after ${TimeoutSec}s waiting for '$Terminator' from Flipper (CLI hung)"
        }

        try {
            $b = $Serial.ReadByte()
            if ($b -ge 0) { $Buffer.Value.Add([byte]$b) }
        } catch [TimeoutException] {
            continue
        } catch [System.IO.IOException] {
            # A known .NET SerialPort quirk over USB CDC: a ReadByte() timeout sometimes
            # surfaces as "The I/O operation has been aborted because of either a thread exit
            # or an application request" (IOException) instead of a clean TimeoutException.
            # Reproduced 2026-09-18 against real hardware mid-transfer. Treated the same as an
            # ordinary timeout -- this loop's own $deadline above still bounds the overall
            # wait, so a genuinely dead connection still surfaces as a clear timeout error
            # rather than looping forever.
            continue
        }
    }
}

# Reads exactly $Count raw bytes, using any leftover buffered bytes first.
function Read-FlipperBytes {
    param($Serial, [ref]$Buffer, [int]$Count, [int]$TimeoutSec = 60)

    $out = New-Object byte[] $Count
    $filled = 0

    $fromBuffer = [Math]::Min($Count, $Buffer.Value.Count)
    if ($fromBuffer -gt 0) {
        $Buffer.Value.CopyTo(0, $out, 0, $fromBuffer)
        $Buffer.Value.RemoveRange(0, $fromBuffer)
        $filled = $fromBuffer
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ($filled -lt $Count) {
        try {
            $n = $Serial.Read($out, $filled, $Count - $filled)
            if ($n -gt 0) { $filled += $n }
        } catch {
            # See Read-FlipperUntil's own comment -- a real timeout can surface as either
            # TimeoutException or IOException from this same underlying SerialPort quirk.
            if ($_.Exception -isnot [TimeoutException] -and $_.Exception -isnot [System.IO.IOException]) {
                throw
            }
            if ((Get-Date) -gt $deadline) {
                throw "Timed out after ${TimeoutSec}s reading $Count bytes from Flipper (CLI hung)"
            }
        }
    }
    return $out
}

function Find-ByteSequence {
    param([System.Collections.Generic.List[byte]]$Haystack, [byte[]]$Needle)
    if ($Needle.Length -eq 0 -or $Haystack.Count -lt $Needle.Length) { return -1 }
    for ($i = 0; $i -le $Haystack.Count - $Needle.Length; $i++) {
        $match = $true
        for ($j = 0; $j -lt $Needle.Length; $j++) {
            if ($Haystack[$i + $j] -ne $Needle[$j]) { $match = $false; break }
        }
        if ($match) { return $i }
    }
    return -1
}

function Send-FlipperRaw {
    param($Serial, [byte[]]$Bytes)
    $Serial.Write($Bytes, 0, $Bytes.Length)
}

function Send-FlipperLine {
    param($Serial, [string]$Line)
    Send-FlipperRaw -Serial $Serial -Bytes ([System.Text.Encoding]::ASCII.GetBytes($Line))
}

function Test-FlipperErrorLine {
    param([string]$Text)
    return $Text.Contains("Storage error:")
}

function Get-FlipperErrorText {
    param([string]$Text)
    $parts = $Text -split ": ", 2
    if ($parts.Length -eq 2) { return $parts[1].Trim() }
    return $Text.Trim()
}

# ---------------------------------------------------------------------------
# storage <cmd> wrappers
# ---------------------------------------------------------------------------

# Returns the file's size in bytes, or -1 if it doesn't exist / isn't a file.
function Get-FlipperFileSize {
    param($Cli, [string]$Path)
    Send-FlipperLine -Serial $Cli.Serial -Line "storage stat `"$Path`"`r"
    # The Flipper's CLI echoes the command line back before printing its actual response --
    # this first read discards that echo (same two-read shape as
    # scripts/flipper/storage.py's send_and_wait_eol()-then-read.until() pattern, e.g. its
    # exist_file()). Missing this made every stat/remove/rename call see its own echoed
    # command instead of the real response (root-caused 2026-09-18 against real hardware).
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol | Out-Null
    $line = Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt | Out-Null
    if ($line -match 'File, size:\s*(\d+)') { return [long]$matches[1] }
    return [long]-1
}

function Test-FlipperFileExists {
    param($Cli, [string]$Path)
    return ((Get-FlipperFileSize -Cli $Cli -Path $Path) -ge 0)
}

function Receive-FlipperFile {
    param($Cli, [string]$FlipperPath, [string]$LocalPath, [int]$ChunkSize = 8192)

    Send-FlipperLine -Serial $Cli.Serial -Line "storage read_chunks `"$FlipperPath`" $ChunkSize`r"
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol | Out-Null # echo
    $sizeLine = Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol
    if (Test-FlipperErrorLine $sizeLine) {
        Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt | Out-Null
        throw "Flipper storage error reading '$FlipperPath': $(Get-FlipperErrorText $sizeLine)"
    }
    $size = [int]($sizeLine -replace '.*:\s*', '')

    $fs = [System.IO.File]::Create($LocalPath)
    try {
        $remaining = $size
        while ($remaining -gt 0) {
            Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator "Ready?$($script:CliEol)" | Out-Null
            Send-FlipperRaw -Serial $Cli.Serial -Bytes ([byte[]]@(0x79)) # 'y', deliberately no trailing \r
            $chunkLen = [Math]::Min($remaining, $ChunkSize)
            $bytes = Read-FlipperBytes -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Count $chunkLen
            $fs.Write($bytes, 0, $bytes.Length)
            $remaining -= $chunkLen
        }
    } finally {
        $fs.Close()
    }
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt | Out-Null
}

# Returns $null on success, else the Storage error reason. `storage remove` refuses an open
# file immediately (FSE_ALREADY_OPEN, storage_process_common_remove) instead of blocking.
function Invoke-FlipperRemove {
    param($Cli, [string]$Path)
    Send-FlipperLine -Serial $Cli.Serial -Line "storage remove `"$Path`"`r"
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol | Out-Null # echo
    $line = Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt | Out-Null
    if (Test-FlipperErrorLine $line) { return (Get-FlipperErrorText $line) }
    return $null
}

function Remove-FlipperFile {
    param($Cli, [string]$Path)
    $err = Invoke-FlipperRemove -Cli $Cli -Path $Path
    if ($err -and $err -ne "file/dir not exist") {
        throw "Flipper storage error removing '$Path': $err"
    }
}

# -Budgeted stops between chunks (never mid-chunk, so the CLI stays in sync) once the run's
# spendable time is gone.
function Send-FlipperFile {
    param($Cli, [string]$LocalPath, [string]$FlipperPath, [int]$ChunkSize = 8192, [switch]$Budgeted)

    if (Test-FlipperFileExists -Cli $Cli -Path $FlipperPath) {
        Remove-FlipperFile -Cli $Cli -Path $FlipperPath
    }

    $bytes = [System.IO.File]::ReadAllBytes($LocalPath)
    $offset = 0
    while ($offset -lt $bytes.Length -or $bytes.Length -eq 0) {
        if ($Budgeted -and (Get-SpendableSec) -le 0) {
            throw "Out of time after writing $offset of $($bytes.Length) bytes to '$FlipperPath'"
        }
        $len = [Math]::Min($ChunkSize, $bytes.Length - $offset)
        Send-FlipperLine -Serial $Cli.Serial -Line "storage write_chunk `"$FlipperPath`" $len`r"
        Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol | Out-Null # echo
        $ack = Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol
        if (Test-FlipperErrorLine $ack) {
            Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt | Out-Null
            throw "Flipper storage error writing '$FlipperPath': $(Get-FlipperErrorText $ack)"
        }
        if ($len -gt 0) {
            $chunk = New-Object byte[] $len
            [Array]::Copy($bytes, $offset, $chunk, 0, $len)
            Send-FlipperRaw -Serial $Cli.Serial -Bytes $chunk
        }
        Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt | Out-Null
        $offset += $len
        if ($bytes.Length -eq 0) { break }
    }
}

# Archives an uploaded file (HP-10/HP-11). Both paths start from a `stat` showing the live file
# still has exactly the uploaded size; any mismatch leaves it untouched (the next publish
# re-sends those rows and wdgwars.pl reports them as duplicates).
#
# Preferred, when the serial copy fits the remaining budget -- copy -> verify -> remove:
#   write the archive from the exact local bytes that were uploaded, re-`stat` the live file,
#   then `remove` it, which fails fast (ALREADY_OPEN) if the FAP has reopened it. Nothing that
#   wasn't uploaded can ever be archived.
#
# Fallback for files too large to copy back over serial in time -- on-device `storage rename`
# with a size-scaled timeout. Accepted because skipping would leave a multi-MB CSV unarchived
# forever (growing, re-uploaded in full every publish). Residual risk: rename is copy+delete of
# the file *as it is when rename runs*, and blocks while the file is open. With HP-09 the FAP
# keeps both files closed for the whole publish, and its 600 s wait outlasts this script's
# budget, so the file can only reopen (and new, never-uploaded rows get archived) if the user
# presses Back mid-run; the post-rename size check reports that case instead of hiding it.
function Complete-FlipperArchive {
    param($Cli, [string]$LocalSnapshot, [string]$LivePath, [string]$ArchivePath, [string]$Label)

    $uploadedSize = [long](Get-Item -LiteralPath $LocalSnapshot).Length
    $keptNote = "Uploaded; new data arrived, $Label kept (re-upload is safe, server skips dups)"
    $copySec = 30 + [Math]::Ceiling($uploadedSize / $script:ArchiveMinBytesPerSec)
    $renameSec = [int](30 + [Math]::Ceiling($uploadedSize / $script:RenameMinBytesPerSec))
    if ((Get-SpendableSec) -lt $renameSec) {
        Write-Host "Not enough time left to archive the $Label (needs ~${renameSec}s) -- leaving it in place."
        return @{ Archived = $false; Note = "Uploaded; out of time, $Label kept (re-upload is safe, server skips dups)" }
    }

    try {
        $liveSize = Get-FlipperFileSize -Cli $Cli -Path $LivePath
        if ($liveSize -ne $uploadedSize) {
            Write-Host "Live $Label is now $liveSize bytes, uploaded $uploadedSize -- new data arrived, not archiving."
            return @{ Archived = $false; Note = $keptNote }
        }
        if ((Get-FlipperFileSize -Cli $Cli -Path $ArchivePath) -ge 0) {
            $ArchivePath = $ArchivePath -replace '(\.[^./]+)$', "_$([guid]::NewGuid().ToString('N').Substring(0, 6))`$1"
        }
    } catch {
        $msg = $_.Exception.Message
        Write-Host "Could not check the live $Label before archiving: $msg -- leaving it in place."
        try { Restore-FlipperCli -Cli $Cli } catch { }
        return @{ Archived = $false; Note = "Uploaded; $Label not archived ($msg)" }
    }

    if ((Get-SpendableSec) -ge $copySec) {
        return (Complete-ArchiveByCopy -Cli $Cli -LocalSnapshot $LocalSnapshot -LivePath $LivePath `
            -ArchivePath $ArchivePath -Label $Label -UploadedSize $uploadedSize -KeptNote $keptNote)
    }
    return (Complete-ArchiveByRename -Cli $Cli -LivePath $LivePath -ArchivePath $ArchivePath -Label $Label `
        -UploadedSize $uploadedSize -TimeoutSec $renameSec)
}

function Complete-ArchiveByCopy {
    param($Cli, [string]$LocalSnapshot, [string]$LivePath, [string]$ArchivePath, [string]$Label,
          [long]$UploadedSize, [string]$KeptNote)

    try {
        Send-FlipperFile -Cli $Cli -LocalPath $LocalSnapshot -FlipperPath $ArchivePath -Budgeted
        $archiveSize = Get-FlipperFileSize -Cli $Cli -Path $ArchivePath
        if ($archiveSize -ne $UploadedSize) {
            throw "archive copy is $archiveSize bytes, expected $UploadedSize"
        }

        $liveSize = Get-FlipperFileSize -Cli $Cli -Path $LivePath
        if ($liveSize -ne $UploadedSize) {
            Write-Host "Live $Label changed to $liveSize bytes while archiving -- keeping it, dropping the archive copy."
            Remove-FlipperFile -Cli $Cli -Path $ArchivePath
            return @{ Archived = $false; Note = $KeptNote }
        }
        $err = Invoke-FlipperRemove -Cli $Cli -Path $LivePath
        if ($err) {
            Write-Host "Could not remove the live ${Label} ($err) -- keeping it, dropping the archive copy."
            Remove-FlipperFile -Cli $Cli -Path $ArchivePath
            if ($err -match 'already open') { return @{ Archived = $false; Note = $KeptNote } }
            return @{ Archived = $false; Note = "Uploaded; $Label not archived ($err)" }
        }
        return @{ Archived = $true; Note = $null; Path = $ArchivePath }
    } catch {
        $msg = $_.Exception.Message
        Write-Host "Archiving the $Label failed: $msg -- the live file was left in place."
        try {
            Restore-FlipperCli -Cli $Cli
            Remove-FlipperFile -Cli $Cli -Path $ArchivePath
        } catch {
            Write-Host "(could not clean up the partial archive copy: $($_.Exception.Message))"
        }
        return @{ Archived = $false; Note = "Uploaded; $Label not archived ($msg)" }
    }
}

# Returns $null on success, else the Storage error reason; throws on a CLI timeout.
function Invoke-FlipperRename {
    param($Cli, [string]$OldPath, [string]$NewPath, [int]$TimeoutSec)
    Send-FlipperLine -Serial $Cli.Serial -Line "storage rename `"$OldPath`" `"$NewPath`"`r"
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol | Out-Null # echo
    $line = Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol -TimeoutSec $TimeoutSec
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt | Out-Null
    if (Test-FlipperErrorLine $line) { return (Get-FlipperErrorText $line) }
    return $null
}

# The outcome is always re-derived from `stat` of both paths afterwards (a timed-out rename may
# still finish on the device), so the report matches what is actually on the SD card.
function Complete-ArchiveByRename {
    param($Cli, [string]$LivePath, [string]$ArchivePath, [string]$Label, [long]$UploadedSize, [int]$TimeoutSec)

    Write-Host "The $Label is too large to copy back over serial in time -- archiving with an on-device rename (timeout ${TimeoutSec}s)..."
    try {
        $err = Invoke-FlipperRename -Cli $Cli -OldPath $LivePath -NewPath $ArchivePath -TimeoutSec $TimeoutSec
        if ($err) {
            Write-Host "Rename refused ($err) -- the live $Label was left in place."
            return @{ Archived = $false; Note = "Uploaded; $Label not archived ($err)" }
        }
    } catch {
        Write-Host "Rename did not answer within ${TimeoutSec}s -- resyncing the CLI to check the outcome..."
        try {
            Restore-FlipperCli -Cli $Cli
        } catch {
            Write-Host "(could not resync the CLI: $($_.Exception.Message))"
            return @{ Archived = $false; Note = "Uploaded; $Label archive outcome unknown (rename timed out)" }
        }
    }

    try {
        $archiveSize = Get-FlipperFileSize -Cli $Cli -Path $ArchivePath
        $liveSize = Get-FlipperFileSize -Cli $Cli -Path $LivePath
    } catch {
        try { Restore-FlipperCli -Cli $Cli } catch { }
        return @{ Archived = $false; Note = "Uploaded; $Label archive outcome unknown ($($_.Exception.Message))" }
    }
    $leaf = ($ArchivePath -split '/')[-1]
    if ($liveSize -lt 0 -and $archiveSize -eq $UploadedSize) {
        return @{ Archived = $true; Note = $null; Path = $ArchivePath }
    }
    if ($liveSize -lt 0 -and $archiveSize -gt $UploadedSize) {
        Write-Host "WARNING: the $Label grew during publish; $leaf holds $($archiveSize - $UploadedSize) bytes that were never uploaded."
        return @{ Archived = $true; Note = "Archived $leaf with rows added after upload - re-upload it"; Path = $ArchivePath }
    }
    if ($liveSize -ge 0 -and $archiveSize -lt 0) {
        return @{ Archived = $false; Note = "Uploaded; rename did not complete, $Label kept (re-upload is safe)" }
    }
    Write-Host "Unexpected state after rename: live $Label $liveSize bytes, $leaf $archiveSize bytes, uploaded $UploadedSize."
    return @{ Archived = $false; Note = "Uploaded; $Label archive state unclear (live $liveSize B, archive $archiveSize B)" }
}

# ---------------------------------------------------------------------------
# Credentials (flat key=value lines, see docs/WARDRIVING_PUBLISH.md)
# ---------------------------------------------------------------------------

function Get-CredentialsMap {
    param([string]$LocalPath)
    $map = @{}
    if (Test-Path $LocalPath) {
        foreach ($line in Get-Content -LiteralPath $LocalPath) {
            if ($line -match '^([^=]+)=(.*)$') { $map[$matches[1]] = $matches[2] }
        }
    }
    return $map
}

function Save-CredentialsMap {
    param([hashtable]$Map, [string]$LocalPath)
    $lines = foreach ($key in $Map.Keys) { "$key=$($Map[$key])" }
    Set-Content -LiteralPath $LocalPath -Value $lines -Encoding ascii
}

function New-CredentialsTempPath {
    return (Join-Path $env:TEMP "wdgwars_credentials_$([guid]::NewGuid()).txt")
}

# The plaintext local copy exists only for the duration of one transfer.
function Read-FlipperCredentials {
    param($Cli)
    if (-not (Test-FlipperFileExists -Cli $Cli -Path $script:CredentialsFlipperPath)) { return @{} }
    $tmp = New-CredentialsTempPath
    try {
        Receive-FlipperFile -Cli $Cli -FlipperPath $script:CredentialsFlipperPath -LocalPath $tmp
        $map = Get-CredentialsMap -LocalPath $tmp
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
    return $map
}

function Save-FlipperCredentials {
    param($Cli, [hashtable]$Map)
    if ($Map.Count -eq 0) {
        Remove-FlipperFile -Cli $Cli -Path $script:CredentialsFlipperPath
        return
    }
    $tmp = New-CredentialsTempPath
    try {
        Save-CredentialsMap -Map $Map -LocalPath $tmp
        Send-FlipperFile -Cli $Cli -LocalPath $tmp -FlipperPath $script:CredentialsFlipperPath
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
}

function Test-ApiKeyFormat {
    param([string]$Key)
    return ($Key -match '^[0-9a-fA-F]{64}$')
}

function Read-ApiKeyFromConsole {
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $secure = Read-Host "Paste your wdgwars.pl API key (from your profile -> API Keys; input is hidden)" -AsSecureString
        $bstr = [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
        try {
            $plain = [System.Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
        } finally {
            [System.Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
            $secure.Dispose()
        }
        $plain = $plain.Trim()
        if (Test-ApiKeyFormat $plain) { return $plain }
        Write-Host "That isn't a wdgwars.pl API key (expected 64 hex characters, got $($plain.Length) characters)."
    }
    throw "No valid wdgwars.pl API key entered (expected 64 hex characters)"
}

# A 401 from the CSV endpoint is its documented "Missing or invalid API key". A 403 counts
# too unless it's an HTML page (proxy/WAF block, says nothing about the key). The mesh
# endpoint also checks an HMAC signature (HP-36: key encoding unconfirmed), so a 401/403 there
# only counts if the body blames the key, never a signature.
function Test-ApiKeyRejected {
    param($Result, [bool]$CsvEndpoint)
    $code = $Result.StatusCode
    if ($code -ne 401 -and $code -ne 403) { return $false }
    $body = [string]$Result.Body
    if ($CsvEndpoint) {
        return ($code -eq 401 -or -not $body.TrimStart().StartsWith("<"))
    }
    return ($body -match '(?i)api[ _-]?key' -and $body -notmatch '(?i)sig|hmac|nonce')
}

# Persists a newly entered key only once wdgwars.pl has accepted it, and drops a stored key
# the server has rejected, so a typo never sticks and a revoked key is re-prompted next run.
function Update-ApiKeyState {
    param($Cli, $Result, [bool]$CsvEndpoint)
    $code = $Result.StatusCode
    if ($code -eq 200 -or $code -eq 202) {
        if ($script:KeyNeedsPersist) {
            try {
                Write-Host "API key accepted -- saving it to the Flipper's SD card..."
                $script:Creds["wdgwars"] = $script:ApiKey
                Save-FlipperCredentials -Cli $Cli -Map $script:Creds
                $script:KeyNeedsPersist = $false
            } catch {
                Write-Host "(could not save the API key to the Flipper: $($_.Exception.Message))"
                Restore-FlipperCli -Cli $Cli
            }
        }
    } elseif (Test-ApiKeyRejected -Result $Result -CsvEndpoint $CsvEndpoint) {
        $script:KeyRejected = $true
        $script:KeyNeedsPersist = $false
        if ($script:KeyWasStored) {
            try {
                Write-Host "wdgwars.pl rejected the stored API key -- removing it from the Flipper; you'll be asked for it next time."
                [void]$script:Creds.Remove("wdgwars")
                Save-FlipperCredentials -Cli $Cli -Map $script:Creds
                $script:KeyWasStored = $false
            } catch {
                Write-Host "(could not remove the rejected API key from the Flipper: $($_.Exception.Message))"
                Restore-FlipperCli -Cli $Cli
            }
        }
    }
}

# ---------------------------------------------------------------------------
# wdgwars.pl HTTP
# ---------------------------------------------------------------------------

function Get-HttpTimeoutSec {
    param([int]$MaxSec)
    return [int][Math]::Max(10, [Math]::Min($MaxSec, (Get-SpendableSec) - 10))
}

# Redirects are never followed (HP-33): HttpWebRequest would resend X-API-Key to wherever the
# Location points. A 3xx comes back as a plain failure result instead.
function Invoke-WdgwarsPost {
    param([string]$Url, [string]$ApiKey, [string]$ContentType, [byte[]]$Body, [int]$TimeoutSec)

    $request = [System.Net.HttpWebRequest]::Create($Url)
    $request.Method = "POST"
    $request.AllowAutoRedirect = $false
    $request.ContentType = $ContentType
    $request.Headers.Add("X-API-Key", $ApiKey)
    $request.ContentLength = $Body.Length
    $request.Timeout = $TimeoutSec * 1000
    $request.ReadWriteTimeout = $TimeoutSec * 1000

    $reqStream = $request.GetRequestStream()
    try {
        $reqStream.Write($Body, 0, $Body.Length)
    } finally {
        $reqStream.Close()
    }

    try {
        $response = $request.GetResponse()
    } catch [System.Net.WebException] {
        $response = $_.Exception.Response
        if (-not $response) { throw }
    }

    try {
        $statusCode = [int]$response.StatusCode
        $location = [string]$response.Headers["Location"]
        $reader = New-Object System.IO.StreamReader($response.GetResponseStream())
        $bodyText = $reader.ReadToEnd()
        $reader.Close()
    } finally {
        $response.Close()
    }

    return @{ StatusCode = $statusCode; Body = $bodyText; Location = $location }
}

function Get-UploadFailureMessage {
    param($Result, $Parsed, [bool]$KeyRejected)
    $code = $Result.StatusCode
    if ($code -ge 300 -and $code -lt 400) {
        $target = if ($Result.Location) { " to $($Result.Location)" } else { "" }
        return "HTTP $code redirect$target refused (not followed)"
    }
    if ($KeyRejected) { return "Invalid API key (HTTP $code) - re-enter it on next publish" }
    $detail = $null
    if ($Parsed -and $Parsed.PSObject.Properties.Name -contains "error") { $detail = [string]$Parsed.error }
    if (-not $detail) { $detail = [string]$Result.Body }
    return "HTTP ${code}: $detail"
}

function Write-ResponseToConsole {
    param($Result)
    Write-Host "wdgwars.pl responded with HTTP $($Result.StatusCode)"
    $body = [string]$Result.Body
    if ($body.Length -gt 500) { $body = $body.Substring(0, 500) + "... ($($Result.Body.Length) chars)" }
    Write-Host $body
}

# Manual multipart body -- no reliance on PS7-only -Form.
function Send-WdgwarsUpload {
    param([string]$Url, [string]$ApiKey, [string]$CsvPath, [int]$TimeoutSec)

    $boundary = [System.Guid]::NewGuid().ToString()
    $fileBytes = [System.IO.File]::ReadAllBytes($CsvPath)
    $fileName = [System.IO.Path]::GetFileName($CsvPath)

    $preamble = "--$boundary`r`n" +
        "Content-Disposition: form-data; name=`"file`"; filename=`"$fileName`"`r`n" +
        "Content-Type: text/csv`r`n`r`n"
    $epilogue = "`r`n--$boundary--`r`n"

    $preambleBytes = [System.Text.Encoding]::UTF8.GetBytes($preamble)
    $epilogueBytes = [System.Text.Encoding]::UTF8.GetBytes($epilogue)
    $body = New-Object byte[] ($preambleBytes.Length + $fileBytes.Length + $epilogueBytes.Length)
    [Array]::Copy($preambleBytes, 0, $body, 0, $preambleBytes.Length)
    [Array]::Copy($fileBytes, 0, $body, $preambleBytes.Length, $fileBytes.Length)
    [Array]::Copy($epilogueBytes, 0, $body, $preambleBytes.Length + $fileBytes.Length, $epilogueBytes.Length)

    return (Invoke-WdgwarsPost -Url $Url -ApiKey $ApiKey -ContentType "multipart/form-data; boundary=$boundary" -Body $body -TimeoutSec $TimeoutSec)
}

# ---------------------------------------------------------------------------
# mesh_log upload (Method 2: JSON + HMAC-SHA256, docs/WARDRIVING_PUBLISH.md
# "Publish-flow integration") -- independent of the CSV upload above: different endpoint,
# different auth scheme (signed body, not just the X-API-Key header), same stored key.
# ---------------------------------------------------------------------------

# Parses flipper/mesh_nodes.h's flat "node_id|network|lat|lon" lines (feb_mesh_log_format_line's
# own wire shape) into wdgwars' `meshcore_nodes` upload entries. Skips (rather than aborts on) a
# malformed line -- same tolerance as the Flipper's own feb_mesh_log_parse_line(), since a
# partially-written or hand-edited file should degrade one record, not the whole publish.
function Get-MeshNodesFromLines {
    param([string[]]$Lines)

    $nodes = @()
    foreach ($line in $Lines) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $parts = $line -split '\|'
        if ($parts.Count -ne 4) {
            Write-Host "Skipping malformed mesh log line: $line"
            continue
        }
        $lat = 0.0
        $lon = 0.0
        # Explicit invariant-culture overload: this project's own lat/lon text is always
        # '.'-separated (feb_mesh_log_format_line's "%.7f"), but plain TryParse(string, [ref])
        # uses the host machine's current culture -- on a comma-decimal locale (confirmed
        # against this session's own bg-BG host) it silently fails to parse every dot-separated
        # value, which would have discarded every mesh node record on such a machine.
        $styles = [System.Globalization.NumberStyles]::Float
        $invariant = [System.Globalization.CultureInfo]::InvariantCulture
        if (-not [double]::TryParse($parts[2], $styles, $invariant, [ref]$lat) -or
            -not [double]::TryParse($parts[3], $styles, $invariant, [ref]$lon)) {
            Write-Host "Skipping mesh log line with unparsable lat/lon: $line"
            continue
        }
        $nodes += [ordered]@{
            node_id = $parts[0]
            network = $parts[1]
            lat     = $lat
            lon     = $lon
        }
    }
    return $nodes
}

# wdgwars.pl's Method 2 (https://wdgwars.pl/help/#api-docs, read 2026-09-27): body is
# {"data": base64(json), "nonce": <16 hex chars>, "sig": hex(HMAC-SHA256(apiKey, nonce+data))},
# POSTed alongside the same X-API-Key header the CSV upload already uses. Only ever sends
# `meshcore_nodes` (never `networks` -- that stays the CSV path's job, see the design doc's
# "Separate HTTP calls" note). Synchronous (no 202/job_id queueing documented for this endpoint,
# unlike the CSV v1/v2 pair), so only HTTP 200 + ok:true counts as confirmed success.
function Send-WdgwarsMeshUpload {
    param([string]$Url, [string]$ApiKey, [array]$MeshNodes, [int]$TimeoutSec)

    # Empty `networks` alongside: wdgwars' Method 2 rejects a body without it as HTTP 400
    # "Invalid data format" (first live run 2026-10-05), even when only meshcore_nodes is set.
    $payload = [ordered]@{ networks = @(); meshcore_nodes = @($MeshNodes) }
    $json = $payload | ConvertTo-Json -Depth 6 -Compress
    $dataB64 = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($json))

    $nonceBytes = New-Object byte[] 8
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($nonceBytes)
    $nonce = -join ($nonceBytes | ForEach-Object { $_.ToString("x2") })

    $hmac = New-Object System.Security.Cryptography.HMACSHA256
    try {
        $hmac.Key = [System.Text.Encoding]::UTF8.GetBytes($ApiKey)
        $sigBytes = $hmac.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($nonce + $dataB64))
    } finally {
        $hmac.Dispose()
    }
    $sig = -join ($sigBytes | ForEach-Object { $_.ToString("x2") })

    $bodyJson = (@{ data = $dataB64; nonce = $nonce; sig = $sig } | ConvertTo-Json -Compress)
    $bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($bodyJson)

    return (Invoke-WdgwarsPost -Url $Url -ApiKey $ApiKey -ContentType "application/json" -Body $bodyBytes -TimeoutSec $TimeoutSec)
}

# ---------------------------------------------------------------------------
# Result file
# ---------------------------------------------------------------------------

function ConvertTo-ResultValue {
    param($Value, [int]$MaxChars)
    $text = [string]$Value
    $text = $text -replace '[\r\n\t]+', ' '
    $text = $text -replace '[^\x20-\x7E]', '?'
    $text = $text.Trim()
    if ($text.Length -gt $MaxChars) { $text = $text.Substring(0, $MaxChars - 3) + "..." }
    return $text
}

# status first (the FAP's parser needs it inside its 512 B read), then the two messages, then
# counts; one line per key, CR/LF stripped, messages capped to the FAP's 96 B buffer, and the
# whole file kept under one 512 B write_chunk. Lines that would overflow are dropped.
function Write-ResultFile {
    param($Cli, [System.Collections.IDictionary]$Fields)

    $priority = @("status", "message", "mesh_status", "mesh_message")
    $keys = @($priority | Where-Object { $Fields.Contains($_) }) +
            @($Fields.Keys | Where-Object { $priority -notcontains $_ })
    $text = New-Object System.Text.StringBuilder
    foreach ($key in $keys) {
        $safeKey = ([string]$key) -replace '[^A-Za-z0-9_]', '_'
        if ($safeKey.Length -gt 40) { $safeKey = $safeKey.Substring(0, 40) }
        $maxChars = if ($safeKey -like "*message") { $script:ResultMessageMaxChars } else { 32 }
        $line = "$safeKey=$(ConvertTo-ResultValue -Value $Fields[$key] -MaxChars $maxChars)`n"
        if ($text.Length + $line.Length -gt $script:ResultMaxBytes) {
            Write-Host "(result file full -- dropping '$safeKey')"
            continue
        }
        [void]$text.Append($line)
    }

    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        [System.IO.File]::WriteAllBytes($tmp, [System.Text.Encoding]::ASCII.GetBytes($text.ToString()))
        Send-FlipperFile -Cli $Cli -LocalPath $tmp -FlipperPath $script:ResultFlipperPath
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------------------
# A-GPS sync (docs/AGPS.md "Build contract"). Pure helpers first -- tested offline by
# scripts/test_agps_sync.ps1, which loads them by name from this file's AST.
# ---------------------------------------------------------------------------

# Flipper `date` argument "%Y-%m-%d %H:%M:%S %u" (cli_main_commands.c): UTC, weekday 1-7
# with Monday = 1 (ISO), so Sunday is 7.
function ConvertTo-AgpsFlipperDateArg {
    param([datetime]$Utc)
    $u = $Utc.ToUniversalTime()
    $dow = [int]$u.DayOfWeek
    if ($dow -eq 0) { $dow = 7 }
    return ($u.ToString("yyyy-MM-dd HH:mm:ss", [System.Globalization.CultureInfo]::InvariantCulture) + " $dow")
}

# Finds "YYYY-MM-DD HH:MM:SS N" anywhere in the CLI output; returns a UTC DateTime or $null.
function ConvertFrom-AgpsFlipperDateText {
    param([string]$Text)
    if ($Text -notmatch '(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2}) (\d)') { return $null }
    try {
        return (New-Object DateTime ([int]$matches[1]), ([int]$matches[2]), ([int]$matches[3]), `
            ([int]$matches[4]), ([int]$matches[5]), ([int]$matches[6]), ([System.DateTimeKind]::Utc))
    } catch {
        return $null
    }
}

function Test-AgpsClockClose {
    param([datetime]$A, [datetime]$B, [int]$ToleranceSec)
    return ([Math]::Abs(($A - $B).TotalSeconds) -le $ToleranceSec)
}

# Validates agps.bin per docs/AGPS.md: 16 B header (magic FAGP, u8 version 1, u8 mask, u16
# window_count @6, u32 generated, u32 reserved), then window_count 16 B entries (u32 from, u32
# to @4, u32 offset @8, u16 length @12, u16 reserved). Every window must lie inside the file.
function Get-AgpsBinInfo {
    param([byte[]]$Bytes, [int]$MaxBytes = 204800)

    $fail = { param($m) return @{ Ok = $false; Error = $m; WindowCount = 0; LastValidTo = [uint32]0 } }
    if ($null -eq $Bytes -or $Bytes.Length -lt 16) { return (& $fail "file shorter than header") }
    if ($Bytes.Length -ge $MaxBytes) { return (& $fail "file too large ($($Bytes.Length) B)") }
    if ($Bytes[0] -ne 0x46 -or $Bytes[1] -ne 0x41 -or $Bytes[2] -ne 0x47 -or $Bytes[3] -ne 0x50) {
        return (& $fail "bad magic")
    }
    if ($Bytes[4] -ne 1) { return (& $fail "unsupported version $($Bytes[4])") }
    $count = [int][BitConverter]::ToUInt16($Bytes, 6)
    if ($count -eq 0) { return (& $fail "no windows") }
    $tableEnd = 16 + 16 * $count
    if ($Bytes.Length -lt $tableEnd) { return (& $fail "window table truncated") }

    $lastTo = [uint32]0
    for ($i = 0; $i -lt $count; $i++) {
        $e = 16 + 16 * $i
        $from = [BitConverter]::ToUInt32($Bytes, $e)
        $to = [BitConverter]::ToUInt32($Bytes, $e + 4)
        $off = [long][BitConverter]::ToUInt32($Bytes, $e + 8)
        $len = [long][BitConverter]::ToUInt16($Bytes, $e + 12)
        if ($to -le $from) { return (& $fail "window $i has an empty range") }
        if ($len -eq 0 -or $len -gt 4096) { return (& $fail "window $i bad length") }
        if ($off -lt $tableEnd -or ($off + $len) -gt $Bytes.Length) { return (& $fail "window $i outside file") }
        $lastTo = $to
    }
    return @{ Ok = $true; Error = $null; WindowCount = $count; LastValidTo = $lastTo }
}

# Exactly one line, as the Flipper app parses it: "OK <window_count> <last_valid_to_unix>" or
# "ERR <short message>" (printable ASCII, no CR/LF).
function Format-AgpsResultLine {
    param($Info, [string]$ErrorMessage)
    if ($Info -and $Info.Ok) { return "OK $($Info.WindowCount) $([uint32]$Info.LastValidTo)" }
    $text = [string]$ErrorMessage
    if (-not $text -and $Info) { $text = [string]$Info.Error }
    $text = ($text -replace '[\r\n\t]+', ' ') -replace '[^\x20-\x7E]', '?'
    $text = $text.Trim()
    if (-not $text) { $text = "failed" }
    if ($text.Length -gt 60) { $text = $text.Substring(0, 57) + "..." }
    return "ERR $text"
}

function Test-AgpsHashIsPlaceholder {
    param([string]$Hash)
    return (-not $Hash -or $Hash -notmatch '^[0-9a-fA-F]{64}$' -or $Hash -match '^0{64}$')
}

function Test-AgpsSha256Match {
    param([string]$Actual, [string]$Expected)
    if (-not $Actual -or -not $Expected) { return $false }
    return ($Actual.Trim().ToLowerInvariant() -eq $Expected.Trim().ToLowerInvariant())
}

function Get-AgpsFileSha256 {
    param([string]$Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        return (-join ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString("x2") }))
    } finally {
        $stream.Close()
        $sha.Dispose()
    }
}

# Returns the path of a hash-verified agps_gen.exe, or throws. -AgpsGenPath skips the check.
function Resolve-AgpsGenerator {
    if ($AgpsGenPath) {
        if (-not (Test-Path -LiteralPath $AgpsGenPath -PathType Leaf)) { throw "AgpsGenPath not found: $AgpsGenPath" }
        Write-Host "WARNING: using a local generator ($AgpsGenPath) -- SHA-256 check SKIPPED. Development use only."
        return (Resolve-Path -LiteralPath $AgpsGenPath).Path
    }
    if (Test-AgpsHashIsPlaceholder $AgpsGenSha256) {
        throw "agps_gen release hash not set in this script (placeholder)"
    }
    $expected = $AgpsGenSha256.Trim().ToLowerInvariant()
    $dir = Join-Path $env:LOCALAPPDATA "flipper-esp32-over-ble\agps_gen\$expected"
    $exe = Join-Path $dir "agps_gen.exe"
    if ((Test-Path -LiteralPath $exe) -and (Test-AgpsSha256Match (Get-AgpsFileSha256 $exe) $expected)) {
        Write-Host "Using cached agps_gen ($AgpsGenTag)."
        return $exe
    }
    [void](New-Item -ItemType Directory -Force -Path $dir)
    Remove-Item -LiteralPath $exe -Force -ErrorAction SilentlyContinue
    $part = Join-Path $dir "download_$([guid]::NewGuid().ToString('N')).part"
    try {
        [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12
        Write-Host "Downloading agps_gen ($AgpsGenTag) from $AgpsGenUrl ..."
        $wc = New-Object System.Net.WebClient
        try { $wc.DownloadFile($AgpsGenUrl, $part) } finally { $wc.Dispose() }
        $actual = Get-AgpsFileSha256 $part
        if (-not (Test-AgpsSha256Match $actual $expected)) {
            throw "agps_gen SHA-256 mismatch (got $actual) - refusing to run it"
        }
        Move-Item -LiteralPath $part -Destination $exe -Force
    } finally {
        Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
    }
    return $exe
}

# Runs the generator in its own process with an empty-but-for-basics environment (no secrets),
# echoing its output to the console. Throws on non-zero exit or timeout.
function Invoke-AgpsGenerator {
    param([string]$ExePath, [string]$OutPath, [string]$CacheDir)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ExePath
    $psi.Arguments = "--out `"$OutPath`" --days $script:AgpsDays --cache `"$CacheDir`""
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.EnvironmentVariables.Clear()
    $sysRoot = [string]$env:SystemRoot
    $psi.EnvironmentVariables["SystemRoot"] = $sysRoot
    $psi.EnvironmentVariables["windir"] = $sysRoot
    $psi.EnvironmentVariables["PATH"] = "$sysRoot\system32;$sysRoot"
    foreach ($name in @("TEMP", "TMP", "LOCALAPPDATA", "APPDATA", "USERPROFILE")) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($value) { $psi.EnvironmentVariables[$name] = $value }
    }

    Write-Host "Running agps_gen ($script:AgpsDays days)..."
    $proc = [System.Diagnostics.Process]::Start($psi)
    try {
        $deadline = (Get-Date).AddSeconds($script:AgpsGenTimeoutSec)
        $outTask = $proc.StandardOutput.ReadLineAsync()
        $errTask = $proc.StandardError.ReadLineAsync()
        $outDone = $false
        $errDone = $false
        while (-not ($outDone -and $errDone)) {
            if ((Get-Date) -gt $deadline) {
                try { $proc.Kill() } catch { }
                throw "agps_gen timed out after $([int]($script:AgpsGenTimeoutSec / 60)) min"
            }
            $progressed = $false
            if (-not $outDone -and $outTask.IsCompleted) {
                $line = $outTask.Result
                if ($null -eq $line) { $outDone = $true } else { Write-Host $line; $outTask = $proc.StandardOutput.ReadLineAsync() }
                $progressed = $true
            }
            if (-not $errDone -and $errTask.IsCompleted) {
                $line = $errTask.Result
                if ($null -eq $line) { $errDone = $true } else { Write-Host "[agps_gen] $line"; $errTask = $proc.StandardError.ReadLineAsync() }
                $progressed = $true
            }
            if (-not $progressed) { Start-Sleep -Milliseconds 50 }
        }
        if (-not $proc.WaitForExit([Math]::Max(1000, [int](($deadline - (Get-Date)).TotalMilliseconds)))) {
            try { $proc.Kill() } catch { }
            throw "agps_gen did not exit"
        }
        if ($proc.ExitCode -ne 0) { throw "agps_gen failed (exit code $($proc.ExitCode))" }
    } finally {
        $proc.Dispose()
    }
}

# Sends `date [args]` and returns the Flipper's reported UTC time (or $null if unparsable).
function Invoke-FlipperDate {
    param($Cli, [string]$SetArg)
    $line = if ($SetArg) { "date $SetArg`r" } else { "date`r" }
    Send-FlipperLine -Serial $Cli.Serial -Line $line
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol | Out-Null # echo
    $text = Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt
    if ($SetArg -and $text -match 'Invalid') { throw "Flipper rejected the date: $($text.Trim())" }
    return (ConvertFrom-AgpsFlipperDateText $text)
}

# Returns $null on success (an already-existing directory counts), else the Storage error.
function Invoke-FlipperMkdir {
    param($Cli, [string]$Path)
    Send-FlipperLine -Serial $Cli.Serial -Line "storage mkdir `"$Path`"`r"
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol | Out-Null # echo
    $line = Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt | Out-Null
    if (Test-FlipperErrorLine $line) {
        $err = Get-FlipperErrorText $line
        if ($err -ne "file/dir already exist") { return $err }
    }
    return $null
}

function Write-AgpsResultFile {
    param($Cli, [string]$Line)
    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        [System.IO.File]::WriteAllBytes($tmp, [System.Text.Encoding]::ASCII.GetBytes($Line + "`n"))
        Send-FlipperFile -Cli $Cli -LocalPath $tmp -FlipperPath $script:AgpsResultFlipperPath
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
}

# Never reads the wdgwars key/credentials. Always attempts to leave one result line on the
# Flipper; never throws (so in -Mode All a failure here can't hide the Publish outcome).
function Invoke-AgpsSync {
    $cli = $null
    $localBin = $null
    $resultLine = $null
    try {
        $agpsPort = $Port
        if ($agpsPort -eq "auto") {
            Write-Host "Looking for the Flipper's CLI serial port..."
            $agpsPort = Find-FlipperPort
            if (-not $agpsPort) { throw "Could not find a Flipper Zero on any serial port." }
        }
        Write-Host "A-GPS sync: using port $agpsPort"
        $cli = Open-FlipperCli -PortName $agpsPort

        $nowUtc = [DateTime]::UtcNow
        Write-Host "Setting the Flipper clock to UTC $($nowUtc.ToString('yyyy-MM-dd HH:mm:ss'))..."
        [void](Invoke-FlipperDate -Cli $cli -SetArg (ConvertTo-AgpsFlipperDateArg $nowUtc))
        $readBack = Invoke-FlipperDate -Cli $cli
        if (-not $readBack -or -not (Test-AgpsClockClose $readBack ([DateTime]::UtcNow) $script:AgpsClockToleranceSec)) {
            throw "Flipper clock verify failed"
        }

        $gen = Resolve-AgpsGenerator
        $cacheDir = Join-Path $env:LOCALAPPDATA "flipper-esp32-over-ble\agps_gen\cache"
        [void](New-Item -ItemType Directory -Force -Path $cacheDir)
        $localBin = Join-Path $env:TEMP "agps_$([guid]::NewGuid().ToString('N')).bin"
        Invoke-AgpsGenerator -ExePath $gen -OutPath $localBin -CacheDir $cacheDir

        if (-not (Test-Path -LiteralPath $localBin)) { throw "generator produced no file" }
        $bytes = [System.IO.File]::ReadAllBytes($localBin)
        $info = Get-AgpsBinInfo -Bytes $bytes -MaxBytes $script:AgpsMaxBytes
        if (-not $info.Ok) { throw "bad agps.bin: $($info.Error)" }
        Write-Host "agps.bin: $($bytes.Length) bytes, $($info.WindowCount) windows."

        $mkErr = Invoke-FlipperMkdir -Cli $cli -Path $AppDataPath
        if ($mkErr) { throw "mkdir failed: $mkErr" }

        Write-Host "Pushing agps.bin to the Flipper..."
        Send-FlipperFile -Cli $cli -LocalPath $localBin -FlipperPath $script:AgpsTmpFlipperPath
        $remoteSize = Get-FlipperFileSize -Cli $cli -Path $script:AgpsTmpFlipperPath
        if ($remoteSize -ne $bytes.Length) { throw "push size mismatch ($remoteSize of $($bytes.Length) B)" }
        Remove-FlipperFile -Cli $cli -Path $script:AgpsFlipperPath
        $renErr = Invoke-FlipperRename -Cli $cli -OldPath $script:AgpsTmpFlipperPath -NewPath $script:AgpsFlipperPath -TimeoutSec 30
        if ($renErr) { throw "rename failed: $renErr" }

        $resultLine = Format-AgpsResultLine -Info $info
        Write-Host "A-GPS sync done: $resultLine"
    } catch {
        $msg = $_.Exception.Message
        Write-Host "A-GPS sync failed: $msg"
        $resultLine = Format-AgpsResultLine -ErrorMessage $msg
    }

    if ($cli) {
        try {
            try { Restore-FlipperCli -Cli $cli } catch { }
            Write-AgpsResultFile -Cli $cli -Line $resultLine
        } catch {
            Write-Host "(could not write the A-GPS result file: $($_.Exception.Message))"
        }
        try { Close-FlipperCli -Cli $cli } catch { }
    }
    if ($localBin) { Remove-Item -LiteralPath $localBin -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

# A previous run killed with its window closed can't run its finally block -- sweep its
# plaintext leftovers (credential copy, CSV/mesh snapshots) before starting.
foreach ($pattern in @("wdgwars_credentials_*.txt", "wardriving_current_*.csv", "mesh_nodes_current_*.txt")) {
    Get-ChildItem -Path $env:TEMP -Filter $pattern -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
}

if ($Mode -ne "Agps") {
    $tempCsv = $null
    $tempMesh = $null
    $cli = $null
    $haveMesh = $null
    $script:Creds = @{}
    $script:ApiKey = $null
    $script:KeyNeedsPersist = $false
    $script:KeyWasStored = $false
    $script:KeyRejected = $false
    # Accumulates fields from both independent operations below into the one result file the
    # Flipper's Publish screen reads (docs/WARDRIVING_PUBLISH.md "One result file, two outcome
    # blocks") -- unprefixed keys are the CSV/wifi-ble outcome, `mesh_`-prefixed keys are the
    # mesh_log outcome. Written exactly once, after both have had a chance to run, so neither
    # operation having nothing to publish (or failing) ever hides the other's real result.
    $resultFields = [ordered]@{}

    try {
        if ($Port -eq "auto") {
            Write-Host "Looking for the Flipper's CLI serial port..."
            $Port = Find-FlipperPort
            if (-not $Port) { throw "Could not find a Flipper Zero on any serial port." }
        }
        Write-Host "Using port $Port"

        $cli = Open-FlipperCli -PortName $Port

        $haveCsv = Test-FlipperFileExists -Cli $cli -Path $script:CsvFlipperPath
        $haveMesh = Test-FlipperFileExists -Cli $cli -Path $script:MeshNodesFlipperPath

        # Credentials are shared between both uploads (docs/WARDRIVING_PUBLISH.md "Separate HTTP
        # calls": mesh reuses "the same 64-char wdgwars API key already stored" -- no new
        # credential), so this is loaded/prompted-for once, before either operation. A newly
        # entered key is only written to the Flipper once wdgwars.pl accepts it.
        if ($haveCsv -or $haveMesh) {
            $script:Creds = Read-FlipperCredentials -Cli $cli
            $storedKey = [string]$script:Creds["wdgwars"]
            if (Test-ApiKeyFormat $storedKey.Trim()) {
                $script:ApiKey = $storedKey.Trim()
                $script:KeyWasStored = $true
            } else {
                Write-Host ""
                if ($storedKey) {
                    Write-Host "The wdgwars.pl API key stored on this Flipper is malformed (expected 64 hex characters)."
                } else {
                    Write-Host "No wdgwars.pl API key stored on this Flipper yet."
                }
                $script:ApiKey = Read-ApiKeyFromConsole
                $script:KeyNeedsPersist = $true
                Write-Host "The key will be saved to the Flipper's SD card once wdgwars.pl accepts it."
            }
        }

        # ---- CSV (wifi/ble) publish -- Method 1 ----
        if (-not $haveCsv) {
            Write-Host "No wardriving CSV found on the Flipper -- nothing to publish."
            $resultFields["status"] = "nothing_to_publish"
        } else {
            $tempCsv = Join-Path $env:TEMP "wardriving_current_$([guid]::NewGuid()).csv"
            Write-Host "Pulling current CSV from the Flipper..."
            Receive-FlipperFile -Cli $cli -FlipperPath $script:CsvFlipperPath -LocalPath $tempCsv

            # WigleWifi-1.6 export is two header lines (metadata line, then column-name line);
            # anything beyond that is real data.
            $csvLineCount = 0
            $csvReader = [System.IO.File]::OpenText($tempCsv)
            try {
                while ($csvLineCount -le 2 -and $null -ne $csvReader.ReadLine()) { $csvLineCount++ }
            } finally {
                $csvReader.Dispose()
            }
            if ($csvLineCount -le 2) {
                Write-Host "CSV has no data rows yet -- nothing to publish."
                $resultFields["status"] = "nothing_to_publish"
            } elseif ((Get-SpendableSec) -lt 20) {
                Write-Host "Out of time before the CSV upload -- leaving it in place."
                $resultFields["status"] = "fail"
                $resultFields["message"] = "Out of time before upload; CSV kept"
            } else {
                try {
                    Write-Host "Uploading CSV to wdgwars.pl..."
                    $result = Send-WdgwarsUpload -Url $UploadUrl -ApiKey $script:ApiKey -CsvPath $tempCsv -TimeoutSec (Get-HttpTimeoutSec 120)
                    Write-ResponseToConsole $result

                    $parsed = $null
                    try { $parsed = $result.Body | ConvertFrom-Json } catch { $parsed = $null }
                    Update-ApiKeyState -Cli $cli -Result $result -CsvEndpoint $true

                    # 202 is wdgwars.pl's async-queued acknowledgement (docs/WARDRIVING_PUBLISH.md's
                    # v2 upload-job behavior) -- confirmed 2026-09-18 that even /api/upload-csv (v1)
                    # can return it under real conditions, not just the documented v2 endpoint.
                    # ok:true here means the server has accepted and queued the upload, which this
                    # project treats as confirmed enough to archive -- an explicit user decision
                    # over waiting on the job to finish via poll_url, which this script does not do.
                    if (($result.StatusCode -eq 200 -or $result.StatusCode -eq 202) -and $parsed -and $parsed.ok -eq $true) {
                        $resultFields["status"] = "ok"
                        foreach ($name in @("imported", "captured", "updated", "duplicates", "no_gps", "bad_rows")) {
                            if ($parsed.PSObject.Properties.Name -contains $name) { $resultFields[$name] = $parsed.$name }
                        }

                        $timestamp = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
                        Write-Host "Upload confirmed -- archiving CSV on the Flipper as $timestamp.csv"
                        $archive = Complete-FlipperArchive -Cli $cli -LocalSnapshot $tempCsv -LivePath $script:CsvFlipperPath `
                            -ArchivePath "$script:WardrivingDir/$timestamp.csv" -Label "CSV"
                        if (-not $archive.Archived) { $resultFields["message"] = $archive.Note }
                    } else {
                        Write-Host "Upload not confirmed as successful -- leaving the CSV in place, nothing archived."
                        $resultFields["status"] = "fail"
                        $resultFields["message"] = Get-UploadFailureMessage -Result $result -Parsed $parsed -KeyRejected $script:KeyRejected
                    }
                } catch {
                    Write-Host "CSV publish failed: $($_.Exception.Message)"
                    $resultFields["status"] = "fail"
                    $resultFields["message"] = $_.Exception.Message
                }
            }
        }

        # ---- Mesh node (MeshCore/Meshtastic) publish -- Method 2, independent of the CSV above:
        # own endpoint, own auth scheme (HMAC-signed body), own archive file, own result-file
        # block. A failure or no-op here never blocks or is blocked by the CSV outcome above. ----
        if (-not $haveMesh) {
            Write-Host "No mesh node log found on the Flipper -- nothing to publish."
            $resultFields["mesh_status"] = "nothing_to_publish"
        } elseif ($script:KeyRejected) {
            Write-Host "Skipping the mesh upload -- wdgwars.pl rejected the API key."
            $resultFields["mesh_status"] = "fail"
            $resultFields["mesh_message"] = "Invalid API key - re-enter it on next publish"
        } elseif ((Get-SpendableSec) -lt 30) {
            Write-Host "Out of time before the mesh upload -- leaving the mesh log in place."
            $resultFields["mesh_status"] = "fail"
            $resultFields["mesh_message"] = "Out of time before upload; mesh log kept"
        } else {
            $tempMesh = Join-Path $env:TEMP "mesh_nodes_current_$([guid]::NewGuid()).txt"
            Write-Host "Pulling mesh node log from the Flipper..."
            Receive-FlipperFile -Cli $cli -FlipperPath $script:MeshNodesFlipperPath -LocalPath $tempMesh

            $meshLines = @(Get-Content -LiteralPath $tempMesh -ErrorAction SilentlyContinue)
            $meshNodes = @(Get-MeshNodesFromLines -Lines $meshLines)

            if ($meshNodes.Count -eq 0) {
                Write-Host "Mesh node log has no valid records -- nothing to publish."
                $resultFields["mesh_status"] = "nothing_to_publish"
            } else {
                try {
                    Write-Host "Uploading $($meshNodes.Count) mesh node(s) to wdgwars.pl..."
                    $result = Send-WdgwarsMeshUpload -Url $MeshUploadUrl -ApiKey $script:ApiKey -MeshNodes $meshNodes -TimeoutSec (Get-HttpTimeoutSec 60)
                    Write-ResponseToConsole $result

                    $parsed = $null
                    try { $parsed = $result.Body | ConvertFrom-Json } catch { $parsed = $null }
                    Update-ApiKeyState -Cli $cli -Result $result -CsvEndpoint $false

                    # Method 2 is synchronous (no job_id/poll_url documented for /api/upload, unlike
                    # the CSV v1/v2 pair) -- only a plain 200 + ok:true counts as confirmed.
                    if ($result.StatusCode -eq 200 -and $parsed -and $parsed.ok -eq $true) {
                        $resultFields["mesh_status"] = "ok"
                        # Real field names confirmed against wdgwars.pl's own docs (read 2026-09-27):
                        # `meshcore_imported` (count) and `meshcore_reject_reasons` (an object of
                        # reason -> count, e.g. bad_network/bad_node_id/no_gps). Flattened into
                        # individual mesh_reject_<reason>=<count> lines rather than stored as a
                        # nested blob -- this firmware's result file is flat key=value text with no
                        # JSON decoder anywhere (same reasoning as the credentials file).
                        if ($parsed.PSObject.Properties.Name -contains "meshcore_imported") {
                            $resultFields["mesh_imported"] = $parsed.meshcore_imported
                        }
                        if ($parsed.PSObject.Properties.Name -contains "meshcore_reject_reasons") {
                            $reasons = $parsed.meshcore_reject_reasons
                            if ($reasons) {
                                foreach ($prop in $reasons.PSObject.Properties) {
                                    $resultFields["mesh_reject_$($prop.Name)"] = $prop.Value
                                }
                            }
                        }

                        $timestamp = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
                        Write-Host "Mesh upload confirmed -- archiving mesh node log on the Flipper as $timestamp.txt"
                        $archive = Complete-FlipperArchive -Cli $cli -LocalSnapshot $tempMesh -LivePath $script:MeshNodesFlipperPath `
                            -ArchivePath "$script:MeshDir/$timestamp.txt" -Label "mesh log"
                        if (-not $archive.Archived) { $resultFields["mesh_message"] = $archive.Note }
                    } else {
                        Write-Host "Mesh upload not confirmed as successful -- leaving the mesh log in place, nothing archived."
                        $resultFields["mesh_status"] = "fail"
                        $resultFields["mesh_message"] = Get-UploadFailureMessage -Result $result -Parsed $parsed -KeyRejected $script:KeyRejected
                    }
                } catch {
                    Write-Host "Mesh publish failed: $($_.Exception.Message)"
                    $resultFields["mesh_status"] = "fail"
                    $resultFields["mesh_message"] = $_.Exception.Message
                }
            }
        }

        Write-ResultFile -Cli $cli -Fields $resultFields
        Write-Host "Done."
    } catch {
        $failure = $_.Exception.Message
        Write-Host "Publish failed: $failure"
        if (-not $resultFields.Contains("status")) {
            $resultFields["status"] = "fail"
            $resultFields["message"] = $failure
        }
        # HP-35: a failure after the CSV step (e.g. reading the mesh log) must not leave the mesh
        # outcome blank next to a status=ok.
        if (-not $resultFields.Contains("mesh_status")) {
            if ($haveMesh -eq $false) {
                $resultFields["mesh_status"] = "nothing_to_publish"
            } else {
                $resultFields["mesh_status"] = "fail"
                $resultFields["mesh_message"] = $failure
            }
        }
        if ($cli) {
            try {
                Restore-FlipperCli -Cli $cli
                Write-ResultFile -Cli $cli -Fields $resultFields
            } catch {
                Write-Host "(could not write a result file back to the Flipper either)"
            }
        }
    } finally {
        if ($cli) { Close-FlipperCli -Cli $cli }
        $script:ApiKey = $null
        $script:Creds = $null
        # Only the transient local copies get deleted -- the SD-persisted credential file is
        # intentionally left on the Flipper (see docs/WARDRIVING_PUBLISH.md, "Credential storage").
        if ($tempCsv -and (Test-Path $tempCsv)) { Remove-Item -LiteralPath $tempCsv -Force -ErrorAction SilentlyContinue }
        if ($tempMesh -and (Test-Path $tempMesh)) { Remove-Item -LiteralPath $tempMesh -Force -ErrorAction SilentlyContinue }
        Get-ChildItem -Path $env:TEMP -Filter "wdgwars_credentials_*.txt" -File -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
}

if ($Mode -ne "Publish") {
    Invoke-AgpsSync
}

# No confirmation gate (docs/WARDRIVING_PUBLISH.md): the script just finishes and returns
# control to the interactive PowerShell prompt the bootstrap opened, leaving output visible
# without requiring a keypress.
Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue
