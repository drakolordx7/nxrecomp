# Downloads the pinned zstd release source into tools\setup\third_party\zstd (only lib\ and LICENSE are kept).
# Idempotent: does nothing when the pinned version is already there. Verifies the tarball's SHA-256 before extracting.
#   powershell -File tools\setup\scripts\fetch_zstd.ps1 [-Force]
param([switch]$Force)
$ErrorActionPreference = 'Stop'

# Pinned release: newest stable zstd on github.com/facebook/zstd (v1.5.7, published 2025-02-19; checked 2026-10-22).
# SHA-256 is the published zstd-1.5.7.tar.gz.sha256 asset of that release, and was also computed from the download.
$Version = '1.5.7'
$Sha256  = 'eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3'
$Url     = "https://github.com/facebook/zstd/releases/download/v$Version/zstd-$Version.tar.gz"

$root  = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$dest  = Join-Path $root 'third_party\zstd'
$stamp = Join-Path $dest '.version'
$want  = "$Version $Sha256"

if (-not $Force -and (Test-Path (Join-Path $dest 'lib\zstd.h')) -and (Test-Path $stamp) -and
    ((Get-Content -Raw $stamp).Trim() -eq $want)) {
    Write-Output "zstd $Version already present in $dest"
    exit 0
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('zstd-fetch-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $tgz = Join-Path $work "zstd-$Version.tar.gz"
    Write-Output "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $tgz -UseBasicParsing
    $got = (Get-FileHash -Algorithm SHA256 -Path $tgz).Hash.ToLowerInvariant()
    if ($got -ne $Sha256) { throw "zstd tarball SHA-256 mismatch: got $got, expected $Sha256. Not extracted." }

    $ex = Join-Path $work 'x'
    New-Item -ItemType Directory -Force -Path $ex | Out-Null
    # Only lib/ and LICENSE are extracted: the tarball's tests/ tree holds symlinks that Windows tar cannot create.
    & tar.exe -xzf $tgz -C $ex "zstd-$Version/lib" "zstd-$Version/LICENSE"
    if ($LASTEXITCODE) { throw "tar failed extracting zstd-$Version (exit $LASTEXITCODE)" }

    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item -Recurse -Force (Join-Path $ex "zstd-$Version\lib") (Join-Path $dest 'lib')
    Copy-Item -Force (Join-Path $ex "zstd-$Version\LICENSE") (Join-Path $dest 'LICENSE')
    if (-not (Test-Path (Join-Path $dest 'lib\zstd.h'))) { throw 'lib\zstd.h missing after extraction' }
    Set-Content -Path $stamp -Value $want -NoNewline
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
Write-Output "zstd $Version (SHA-256 $Sha256) installed in $dest"
