# Computes the build-verify skill's default target list ("all changed") from git state:
# tracked diff vs HEAD plus untracked files. Prints one target name per line (esp32, esp32c5,
# heltec, flipper, hosttests, shared), or nothing if no shared-file/board-file change is found.
#
# Mapping: a board directory's own files select just that board; anything under
# components/feb_protocol/ (the cross-board shared component) selects every board plus
# hosttests and shared, since a shared-code change is exactly the case check_shared_headers.py
# and every board's build need to re-validate; anything under tests/ selects hosttests only.
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir "..\..\..\..")

Push-Location $repoRoot
try {
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $tracked = git diff --name-only HEAD 2>$null
    $untracked = git ls-files --others --exclude-standard 2>$null
    $ErrorActionPreference = $savedEap
} finally {
    Pop-Location
}

$changed = @($tracked) + @($untracked) | Where-Object { $_ } | ForEach-Object { $_ -replace '\\', '/' }

$targets = New-Object System.Collections.Generic.HashSet[string]
foreach ($f in $changed) {
    if ($f -match '^components/feb_protocol/') {
        foreach ($t in @("esp32", "esp32c5", "heltec", "flipper", "hosttests", "shared")) {
            [void]$targets.Add($t)
        }
    } elseif ($f -match '^esp32c5/') {
        [void]$targets.Add("esp32c5")
    } elseif ($f -match '^esp32/') {
        [void]$targets.Add("esp32")
    } elseif ($f -match '^heltec/') {
        [void]$targets.Add("heltec")
    } elseif ($f -match '^flipper/') {
        [void]$targets.Add("flipper")
    } elseif ($f -match '^tests/') {
        [void]$targets.Add("hosttests")
    }
}

$targets | Sort-Object
