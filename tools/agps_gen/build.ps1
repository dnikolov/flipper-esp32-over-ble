$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) { throw "csc.exe not found at $csc" }
$outDir = Join-Path $here 'out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir 'agps_gen.exe'
$srcs = Get-ChildItem -Path $here -Filter '*.cs' | Sort-Object Name | ForEach-Object { $_.FullName }
# Old csc has no /deterministic: the exe bytes differ per build (MVID); the generated agps.bin is what is deterministic.
& $csc /nologo /target:exe /optimize+ /debug- /langversion:5 /platform:anycpu "/out:$exe" @srcs
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
$hash = (Get-FileHash -Algorithm SHA256 -Path $exe).Hash.ToLowerInvariant()
Write-Host "built $exe"
Write-Host "sha256 $hash"
