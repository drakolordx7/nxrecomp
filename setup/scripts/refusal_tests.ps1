# Refusal oracle on the real dump: the CLI must refuse every bad input with the right exit code, a plain message, and without
# writing any game file.
#   pwsh -File tools\setup\scripts\refusal_tests.ps1 -Dump <real .nsp> -Keys <prod.keys> [-Work <scratch folder>] [-Cli <exe>]
# Needs about 8 GB of scratch space (a corrupted copy of the dump). Exit code = number of failed cases.
param(
    [Parameter(Mandatory)] [string] $Dump,
    [Parameter(Mandatory)] [string] $Keys,
    [string] $Work = (Join-Path $PSScriptRoot '..\..\..\work\setup_test\refusal'),
    [string] $Cli = (Join-Path $PSScriptRoot '..\build\release\bin\mpr_setup_cli.exe')
)
$ErrorActionPreference = 'Stop'
$Work = [IO.Path]::GetFullPath($Work)
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force $Work | Out-Null
$script:fail = 0

function Run-Case([string]$name, [int]$want, [string]$match, [string[]]$cliArgs, [string]$out) {
    New-Item -ItemType Directory -Force $out | Out-Null
    $text = & $Cli @cliArgs --out $out 2>&1 | Out-String
    $code = $LASTEXITCODE
    $game = @(Get-ChildItem $out -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match '\\(exefs|romfs|bc7_cache)\\' }).Count
    $ok = ($code -eq $want) -and ($text -match $match) -and ($game -eq 0)
    $msg = ($text -split "`n" | Where-Object { $_.Trim() -and $_ -notmatch '^\s*(Keys|Dump|Game|Checking|---)' } | Select-Object -Last 1).Trim()
    if ($ok) { "PASS  $name  (exit $code, no game files)  -> $msg" } else { $script:fail++; "FAIL  $name  exit $code (wanted $want), game files written: $game`n      $text" }
}

# 1 garbage file
$garbage = Join-Path $Work 'garbage.nsp'
[IO.File]::WriteAllBytes($garbage, [byte[]](1..4096 | ForEach-Object { [byte]($_ * 31 % 251) }))
Run-Case 'garbage file' 2 'not a Switch game dump' @('--input', $garbage, '--keys', $Keys) (Join-Path $Work 'o1')

# 2 file that does not exist
Run-Case 'missing file' 2 'Cannot open' @('--input', (Join-Path $Work 'nope.nsp'), '--keys', $Keys) (Join-Path $Work 'o2')

# 3 another game (title id mismatch)
Run-Case 'different game' 2 'different game|does not contain' @('--input', $Dump, '--keys', $Keys, '--title-id', '0100000000010000') (Join-Path $Work 'o3')

# 4 other version (build id mismatch)
Run-Case 'other version' 2 'not the v1\.0\.0' @('--input', $Dump, '--keys', $Keys, '--build-id', '0000000000000000000000000000000000000000') (Join-Path $Work 'o4')

# 5 keys that do not belong to the dump: random values for the derived keys (no real key material is written)
$rnd = New-Object Random 7
function Hex([int]$n) { -join (1..$n | ForEach-Object { '{0:x2}' -f $rnd.Next(256) }) }
$fake = Join-Path $Work 'fake_prod.keys'
@("header_key = $(Hex 32)", "key_area_key_application_0a = $(Hex 16)", "titlekek_0a = $(Hex 16)", "master_key_0a = $(Hex 16)") | Set-Content $fake
Run-Case 'wrong keys' 3 'keys cannot open|cannot open this dump' @('--input', $Dump, '--keys', $fake) (Join-Path $Work 'o5')

# 6 truncated dump (first 3 GB)
$trunc = Join-Path $Work 'truncated.nsp'
$in = [IO.File]::OpenRead($Dump); $outf = [IO.File]::Create($trunc)
$buf = New-Object byte[] (16MB); $left = 3GB
while ($left -gt 0) { $n = $in.Read($buf, 0, [int][Math]::Min([long]$buf.Length, [long]$left)); if ($n -le 0) { break }; $outf.Write($buf, 0, $n); $left -= $n }
$in.Close(); $outf.Close()
Run-Case 'truncated dump' 2 'cut short|shorter|past the end|damaged|incomplete' @('--input', $trunc, '--keys', $Keys) (Join-Path $Work 'o6')
Remove-Item $trunc

# 7 one flipped byte in the RomFS data (full copy, byte at 3 GB)
$bad = Join-Path $Work 'flipped.nsp'
Copy-Item -LiteralPath $Dump -Destination $bad
$fs = [IO.File]::Open($bad, 'Open', 'ReadWrite'); $fs.Seek(3GB, 'Begin') | Out-Null
$b = $fs.ReadByte(); $fs.Seek(3GB, 'Begin') | Out-Null; $fs.WriteByte([byte]($b -bxor 0x40)); $fs.Close()
Run-Case 'one flipped byte' 2 'damaged' @('--input', $bad, '--keys', $Keys) (Join-Path $Work 'o7')
Remove-Item $bad

"`n$($script:fail) case(s) failed"
exit $script:fail
