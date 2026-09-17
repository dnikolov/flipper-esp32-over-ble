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
    [string]$UploadUrl = "https://wdgwars.pl/api/upload-csv"
)

$ErrorActionPreference = "Stop"

$script:CliPrompt = ">: "
$script:CliEol = "`r`n"
# The wardriving CSV (current + archived) lives in its own "wardriving" subdirectory
# alongside the FAP's older per-calendar-day export files; the credentials and result files
# are publish-flow plumbing, not wardriving data, and stay flat at the app data root.
$script:WardrivingDir = "$AppDataPath/wardriving"
$script:CsvFlipperPath = "$script:WardrivingDir/wardriving_current.csv"
$script:CredentialsFlipperPath = "$AppDataPath/wdgwars_credentials.txt"
$script:ResultFlipperPath = "$AppDataPath/wardriving_publish_result.txt"

# ---------------------------------------------------------------------------
# Low-level CLI-over-serial client
# ---------------------------------------------------------------------------

# Retries the whole port scan, not just a single port open, for up to $TimeoutSec --
# immediately after BadUSB types the bootstrap, the Flipper is still switching its USB
# personality back from HID to its normal CDC/serial mode, and Windows can take several
# seconds to detach the HID device, load the CDC driver, and assign a COM port. A single
# GetPortNames() snapshot taken right as the script starts can easily miss it entirely, since
# the port doesn't exist yet at that instant, not just fail to open (real-world failure seen
# 2026-09-18: the script started and failed in well under a second, before the Flipper had
# finished re-enumerating).
function Find-FlipperPort {
    param([int]$TimeoutSec = 20)

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $printedWaitMessage = $false
    while ((Get-Date) -lt $deadline) {
        $candidates = [System.IO.Ports.SerialPort]::GetPortNames()
        foreach ($name in $candidates) {
            try {
                $probe = New-Object System.IO.Ports.SerialPort $name, 115200
                $probe.ReadTimeout = 1500
                $probe.WriteTimeout = 1500
                $probe.Open()
                Start-Sleep -Milliseconds 300
                $buffer = New-Object System.Text.StringBuilder
                $isFlipper = $false
                $probeDeadline = (Get-Date).AddSeconds(2)
                while ((Get-Date) -lt $probeDeadline) {
                    try {
                        $b = $probe.ReadByte()
                        if ($b -ge 0) { [void]$buffer.Append([char]$b) }
                        if ($buffer.ToString().Contains($script:CliPrompt)) { $isFlipper = $true; break }
                    } catch [TimeoutException] { break }
                }
                $probe.Close()
                if ($isFlipper) { return $name }
            } catch {
                continue
            }
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
    $serial.Open()
    Start-Sleep -Milliseconds 500

    $buffer = New-Object System.Collections.Generic.List[byte]
    Read-FlipperUntil -Serial $serial -Buffer ([ref]$buffer) -Terminator $script:CliPrompt -TimeoutSec 10 | Out-Null
    $serial.DiscardInBuffer()

    # Flush any stray input with a known-syntax command, matching storage.py's own startup sync.
    Send-FlipperLine -Serial $serial -Line "device_info`r"
    Read-FlipperUntil -Serial $serial -Buffer ([ref]$buffer) -Terminator "hardware_model" -TimeoutSec 10 | Out-Null
    Read-FlipperUntil -Serial $serial -Buffer ([ref]$buffer) -Terminator $script:CliPrompt -TimeoutSec 10 | Out-Null

    return @{ Serial = $serial; Buffer = $buffer }
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
        } catch [TimeoutException] {
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

function Test-FlipperFileExists {
    param($Cli, [string]$Path)
    Send-FlipperLine -Serial $Cli.Serial -Line "storage stat `"$Path`"`r"
    $line = Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt | Out-Null
    return ($line -match "File, size:")
}

function Receive-FlipperFile {
    param($Cli, [string]$FlipperPath, [string]$LocalPath, [int]$ChunkSize = 8192)

    Send-FlipperLine -Serial $Cli.Serial -Line "storage read_chunks `"$FlipperPath`" $ChunkSize`r"
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

function Remove-FlipperFile {
    param($Cli, [string]$Path)
    Send-FlipperLine -Serial $Cli.Serial -Line "storage remove `"$Path`"`r"
    $line = Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt | Out-Null
    if ((Test-FlipperErrorLine $line) -and (Get-FlipperErrorText $line) -ne "file/dir not exist") {
        throw "Flipper storage error removing '$Path': $(Get-FlipperErrorText $line)"
    }
}

function Send-FlipperFile {
    param($Cli, [string]$LocalPath, [string]$FlipperPath, [int]$ChunkSize = 8192)

    if (Test-FlipperFileExists -Cli $Cli -Path $FlipperPath) {
        Remove-FlipperFile -Cli $Cli -Path $FlipperPath
    }

    $bytes = [System.IO.File]::ReadAllBytes($LocalPath)
    $offset = 0
    while ($offset -lt $bytes.Length -or $bytes.Length -eq 0) {
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

function Rename-FlipperFile {
    param($Cli, [string]$OldPath, [string]$NewPath)
    Send-FlipperLine -Serial $Cli.Serial -Line "storage rename `"$OldPath`" `"$NewPath`"`r"
    $line = Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliEol
    Read-FlipperUntil -Serial $Cli.Serial -Buffer ([ref]$Cli.Buffer) -Terminator $script:CliPrompt | Out-Null
    if (Test-FlipperErrorLine $line) {
        throw "Flipper storage error renaming '$OldPath' -> '$NewPath': $(Get-FlipperErrorText $line)"
    }
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

# ---------------------------------------------------------------------------
# wdgwars.pl upload (manual multipart body -- no reliance on PS7-only -Form)
# ---------------------------------------------------------------------------

function Send-WdgwarsUpload {
    param([string]$Url, [string]$ApiKey, [string]$CsvPath)

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

    $request = [System.Net.HttpWebRequest]::Create($Url)
    $request.Method = "POST"
    $request.ContentType = "multipart/form-data; boundary=$boundary"
    $request.Headers.Add("X-API-Key", $ApiKey)
    $request.ContentLength = $body.Length
    $request.Timeout = 120000

    $reqStream = $request.GetRequestStream()
    $reqStream.Write($body, 0, $body.Length)
    $reqStream.Close()

    try {
        $response = $request.GetResponse()
    } catch [System.Net.WebException] {
        $response = $_.Exception.Response
        if (-not $response) { throw }
    }

    $statusCode = [int]$response.StatusCode
    $stream = $response.GetResponseStream()
    $reader = New-Object System.IO.StreamReader($stream)
    $bodyText = $reader.ReadToEnd()
    $reader.Close()
    $response.Close()

    return @{ StatusCode = $statusCode; Body = $bodyText }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

function Write-ResultFile {
    param($Cli, [hashtable]$Fields)
    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        $lines = foreach ($key in $Fields.Keys) { "$key=$($Fields[$key])" }
        Set-Content -LiteralPath $tmp -Value $lines -Encoding ascii
        Send-FlipperFile -Cli $Cli -LocalPath $tmp -FlipperPath $script:ResultFlipperPath
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
}

$tempCsv = $null
$tempCreds = $null
$cli = $null

try {
    if ($Port -eq "auto") {
        Write-Host "Looking for the Flipper's CLI serial port..."
        $Port = Find-FlipperPort
        if (-not $Port) { throw "Could not find a Flipper Zero on any serial port." }
    }
    Write-Host "Using port $Port"

    $cli = Open-FlipperCli -PortName $Port

    if (-not (Test-FlipperFileExists -Cli $cli -Path $script:CsvFlipperPath)) {
        Write-Host "No wardriving CSV found on the Flipper -- nothing to publish."
        Write-ResultFile -Cli $cli -Fields @{ status = "nothing_to_publish" }
        return
    }

    $tempCsv = Join-Path $env:TEMP "wardriving_current_$([guid]::NewGuid()).csv"
    Write-Host "Pulling current CSV from the Flipper..."
    Receive-FlipperFile -Cli $cli -FlipperPath $script:CsvFlipperPath -LocalPath $tempCsv

    $csvLines = Get-Content -LiteralPath $tempCsv
    # WigleWifi-1.6 export is two header lines (metadata line, then column-name line); anything
    # beyond that is real data.
    if ($csvLines.Count -le 2) {
        Write-Host "CSV has no data rows yet -- nothing to publish."
        Write-ResultFile -Cli $cli -Fields @{ status = "nothing_to_publish" }
        return
    }

    $tempCreds = Join-Path $env:TEMP "wdgwars_credentials_$([guid]::NewGuid()).txt"
    $haveCreds = Test-FlipperFileExists -Cli $cli -Path $script:CredentialsFlipperPath
    if ($haveCreds) {
        Receive-FlipperFile -Cli $cli -FlipperPath $script:CredentialsFlipperPath -LocalPath $tempCreds
    }
    $creds = Get-CredentialsMap -LocalPath $tempCreds

    if (-not $creds.ContainsKey("wdgwars") -or [string]::IsNullOrWhiteSpace($creds["wdgwars"])) {
        Write-Host ""
        Write-Host "No wdgwars.pl API key stored on this Flipper yet."
        $apiKey = Read-Host "Paste your wdgwars.pl API key (from your profile -> API Keys)"
        $creds["wdgwars"] = $apiKey.Trim()
        Save-CredentialsMap -Map $creds -LocalPath $tempCreds
        Write-Host "Saving key to the Flipper's SD card..."
        Send-FlipperFile -Cli $cli -LocalPath $tempCreds -FlipperPath $script:CredentialsFlipperPath
    }

    Write-Host "Uploading to wdgwars.pl..."
    $result = Send-WdgwarsUpload -Url $UploadUrl -ApiKey $creds["wdgwars"] -CsvPath $tempCsv
    Write-Host "wdgwars.pl responded with HTTP $($result.StatusCode)"
    Write-Host $result.Body

    $parsed = $null
    try { $parsed = $result.Body | ConvertFrom-Json } catch { $parsed = $null }

    if ($result.StatusCode -eq 200 -and $parsed -and $parsed.ok -eq $true) {
        $timestamp = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
        $archivePath = "$script:WardrivingDir/$timestamp.csv"
        Write-Host "Upload confirmed -- archiving CSV on the Flipper as $timestamp.csv"
        Rename-FlipperFile -Cli $cli -OldPath $script:CsvFlipperPath -NewPath $archivePath

        $fields = @{ status = "ok" }
        foreach ($name in @("imported", "captured", "updated", "duplicates", "no_gps", "bad_rows")) {
            if ($parsed.PSObject.Properties.Name -contains $name) { $fields[$name] = $parsed.$name }
        }
        Write-ResultFile -Cli $cli -Fields $fields
        Write-Host "Done."
    } else {
        Write-Host "Upload not confirmed as successful -- leaving the CSV in place, nothing renamed."
        Write-ResultFile -Cli $cli -Fields @{
            status  = "fail"
            message = "HTTP $($result.StatusCode): $($result.Body)"
        }
    }
} catch {
    Write-Host "Publish failed: $($_.Exception.Message)"
    if ($cli) {
        try {
            Write-ResultFile -Cli $cli -Fields @{ status = "fail"; message = $_.Exception.Message }
        } catch {
            Write-Host "(could not write a result file back to the Flipper either)"
        }
    }
} finally {
    if ($cli) { Close-FlipperCli -Cli $cli }
    # Only the transient local copies get deleted -- the SD-persisted credential file is
    # intentionally left on the Flipper (see docs/WARDRIVING_PUBLISH.md, "Credential storage").
    if ($tempCsv -and (Test-Path $tempCsv)) { Remove-Item -LiteralPath $tempCsv -Force -ErrorAction SilentlyContinue }
    if ($tempCreds -and (Test-Path $tempCreds)) { Remove-Item -LiteralPath $tempCreds -Force -ErrorAction SilentlyContinue }
    # No confirmation gate (docs/WARDRIVING_PUBLISH.md): the script just finishes and returns
    # control to the interactive PowerShell prompt the bootstrap opened, leaving output visible
    # without requiring a keypress.
    Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue
}
