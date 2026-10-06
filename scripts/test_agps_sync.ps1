# Offline self-test for the pure A-GPS helpers in publish_wardriving.ps1. No serial port, no
# network, no Flipper. Loads the helper functions by name from the script's AST (the script
# itself is never run). Exit code 0 = all PASS.

$ErrorActionPreference = "Stop"
$scriptPath = Join-Path $PSScriptRoot "publish_wardriving.ps1"

$ast = [System.Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$null, [ref]$null)
$wanted = @("ConvertTo-AgpsFlipperDateArg", "ConvertFrom-AgpsFlipperDateText", "Test-AgpsClockClose",
            "Get-AgpsBinInfo", "Format-AgpsResultLine", "Test-AgpsHashIsPlaceholder", "Test-AgpsSha256Match")
foreach ($name in $wanted) {
    $fn = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name }, $true)
    if (-not $fn) { Write-Host "FAIL: function $name not found"; exit 1 }
    . ([scriptblock]::Create($fn.Extent.Text))
}

$script:failures = 0
function Check {
    param([string]$Name, $Actual, $Expected)
    if ("$Actual" -eq "$Expected") { Write-Host "PASS  $Name" }
    else { Write-Host "FAIL  $Name (got '$Actual', expected '$Expected')"; $script:failures++ }
}

# Synthetic agps.bin: header + N windows (16 B each) + payload bytes per window.
function New-Bin {
    param([int]$Windows = 3, [int]$WinLen = 40, [byte[]]$Magic = @(0x46, 0x41, 0x47, 0x50), [byte]$Version = 1,
          [uint32]$From0 = 1790000000, [int]$Pad = 0)
    $tableEnd = 16 + 16 * $Windows
    $ms = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $ms
    $w.Write($Magic); $w.Write([byte]$Version); $w.Write([byte]1)
    $w.Write([uint16]$Windows); $w.Write([uint32]1790000000); $w.Write([uint32]0)
    for ($i = 0; $i -lt $Windows; $i++) {
        $from = [uint32]($From0 + 14400 * $i)
        $w.Write($from); $w.Write([uint32]($from + 14400))
        $w.Write([uint32]($tableEnd + $WinLen * $i)); $w.Write([uint16]$WinLen); $w.Write([uint16]0)
    }
    $w.Write((New-Object byte[] ($WinLen * $Windows)))
    if ($Pad -gt 0) { $w.Write((New-Object byte[] $Pad)) }
    $w.Flush()
    return ,$ms.ToArray()
}

$good = New-Bin
$info = Get-AgpsBinInfo -Bytes $good
Check "valid: Ok" $info.Ok $true
Check "valid: window_count" $info.WindowCount 3
Check "valid: last_valid_to" $info.LastValidTo ([uint32](1790000000 + 14400 * 2 + 14400))
Check "valid: result line" (Format-AgpsResultLine -Info $info) "OK 3 1790043200"

$trunc = $good[0..($good.Length - 30)]
Check "truncated payload: rejected" (Get-AgpsBinInfo -Bytes $trunc).Ok $false
Check "truncated table: rejected" (Get-AgpsBinInfo -Bytes $good[0..40]).Ok $false
Check "shorter than header: rejected" (Get-AgpsBinInfo -Bytes $good[0..9]).Ok $false
Check "empty: rejected" (Get-AgpsBinInfo -Bytes ([byte[]]@())).Ok $false
Check "bad magic: rejected" (Get-AgpsBinInfo -Bytes (New-Bin -Magic @(0x58, 0x41, 0x47, 0x50))).Ok $false
Check "bad magic: error" (Get-AgpsBinInfo -Bytes (New-Bin -Magic @(0x58, 0x41, 0x47, 0x50))).Error "bad magic"
Check "version 2: rejected" (Get-AgpsBinInfo -Bytes (New-Bin -Version 2)).Ok $false
$zero = New-Bin -Windows 0
Check "window_count 0: rejected" (Get-AgpsBinInfo -Bytes $zero).Ok $false
Check "window_count 0: error" (Get-AgpsBinInfo -Bytes $zero).Error "no windows"
$big = New-Bin -Pad 204800
Check "oversize: rejected" (Get-AgpsBinInfo -Bytes $big).Ok $false
Check "just under limit: accepted" (Get-AgpsBinInfo -Bytes (New-Bin -Pad (204799 - $good.Length))).Ok $true

Check "ERR line" (Format-AgpsResultLine -ErrorMessage "generator failed (exit code 3)") "ERR generator failed (exit code 3)"
Check "ERR line strips CRLF/non-ASCII" (Format-AgpsResultLine -ErrorMessage "a`r`nb$([char]0xE9)") "ERR a b?"
Check "ERR line from failed info" (Format-AgpsResultLine -Info $trunc.Length) "ERR failed"
Check "ERR line caps length" (Format-AgpsResultLine -ErrorMessage ("x" * 200)).Length 64
Check "ERR line from info.Error" (Format-AgpsResultLine -Info (Get-AgpsBinInfo -Bytes $zero)) "ERR no windows"

$h = "ABCDEF0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789"
Check "sha compare case-insensitive" (Test-AgpsSha256Match $h $h.ToLowerInvariant()) $true
Check "sha compare mismatch" (Test-AgpsSha256Match $h ("0" * 63 + "1")) $false
Check "sha compare empty" (Test-AgpsSha256Match "" $h) $false
Check "placeholder: zeros" (Test-AgpsHashIsPlaceholder ("0" * 64)) $true
Check "placeholder: empty" (Test-AgpsHashIsPlaceholder "") $true
Check "placeholder: short" (Test-AgpsHashIsPlaceholder "abc") $true
Check "placeholder: real hash" (Test-AgpsHashIsPlaceholder $h) $false

# 2026-10-06 is a Tuesday (ISO 2); 2026-10-04 a Sunday (ISO 7, not 0). Local-offset input must
# be converted to UTC.
Check "date arg Tuesday" (ConvertTo-AgpsFlipperDateArg ([DateTime]::new(2026, 10, 6, 9, 5, 3, [DateTimeKind]::Utc))) "2026-10-06 09:05:03 2"
Check "date arg Sunday" (ConvertTo-AgpsFlipperDateArg ([DateTime]::new(2026, 10, 4, 23, 59, 59, [DateTimeKind]::Utc))) "2026-10-04 23:59:59 7"
Check "date arg Monday" (ConvertTo-AgpsFlipperDateArg ([DateTime]::new(2026, 10, 5, 0, 0, 0, [DateTimeKind]::Utc))) "2026-10-05 00:00:00 1"
$local = [DateTime]::new(2026, 10, 6, 9, 0, 0, [DateTimeKind]::Utc).ToLocalTime()
Check "date arg from local kind" (ConvertTo-AgpsFlipperDateArg $local) "2026-10-06 09:00:00 2"

$parsed = ConvertFrom-AgpsFlipperDateText "New datetime is: 2026-10-06 09:05:03 2"
Check "date parse" $parsed.ToString("yyyy-MM-dd HH:mm:ss") "2026-10-06 09:05:03"
Check "date parse kind" $parsed.Kind "Utc"
Check "date parse garbage" ($null -eq (ConvertFrom-AgpsFlipperDateText "Invalid datetime data")) $true
Check "date parse invalid month" ($null -eq (ConvertFrom-AgpsFlipperDateText "2026-13-06 09:05:03 2")) $true
Check "clock close in tolerance" (Test-AgpsClockClose $parsed $parsed.AddSeconds(4) 5) $true
Check "clock close out of tolerance" (Test-AgpsClockClose $parsed $parsed.AddSeconds(-6) 5) $false

if ($script:failures -gt 0) { Write-Host "`n$($script:failures) check(s) FAILED"; exit 1 }
Write-Host "`nAll checks passed"
exit 0
