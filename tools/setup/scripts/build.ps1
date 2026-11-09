# Configure + build the setup tool (library, CLI, GUI, tests) with CMake + Ninja + clang-cl, then optionally run the tests.
#   pwsh -File tools\setup\scripts\build.ps1 -Game <games\<id>> [-Clean] [-Test] [-Target name] [-BuildDir path] [-Version 0.1.0]
# -Game: the game folder whose manifest (game.json) the tool is built for (default NX_GAME_DIR).
# Needs tools\textures\third_party (fetch_third_party.ps1 there). Output: <BuildDir>\bin\ (default <NX>\build\setup-<id>)
param([string]$Game = $env:NX_GAME_DIR, [switch]$Clean, [switch]$Test, [string]$Target = '', [string]$BuildDir = '', [string]$Version = 'dev')
$ErrorActionPreference = 'Stop'
$root  = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Game) { throw 'pass -Game <game folder> (or set NX_GAME_DIR)' }
$Game = [IO.Path]::GetFullPath($Game)
$id = (Get-Content -Raw (Join-Path $Game 'game.json') | ConvertFrom-Json).id
# NX_ROOT: the folder holding games\ and build\ (default: the nearest parent of this checkout with a games folder)
$nx = $env:NX_ROOT
if (-not $nx) { $d = $root; while ($d -and -not (Test-Path (Join-Path $d 'games'))) { $d = Split-Path $d -Parent }; $nx = $d }
$build = if ($BuildDir) { $BuildDir } elseif ($nx) { Join-Path $nx "build\setup-$id" } else { Join-Path $root "build\$id" }
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
foreach ($line in (cmd /c "`"$vcvars`" >nul 2>&1 && set")) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { Set-Item -Path ("Env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
}
$env:PATH = "C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;$env:LOCALAPPDATA\Microsoft\WinGet\Links;$env:PATH"
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DNXS_VERSION="$Version" -DNX_GAME_DIR="$Game"
if ($LASTEXITCODE) { throw 'cmake configure failed' }
if ($Target) { cmake --build $build --target $Target } else { cmake --build $build }
if ($LASTEXITCODE) { throw 'build failed' }
if ($Test) {
    ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE) { throw 'tests failed' }
}
Write-Output "built: $build\bin"
