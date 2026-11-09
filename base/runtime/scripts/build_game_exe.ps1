# Configure + build one game's exe (and launcher with -Release): the base runtime is compiled in, the lifted libraries are imported.
#   pwsh runtime\scripts\build_game_exe.ps1 -Game <games\<id>> [-Release] [-Version 0.1.0] [-LibDir <dir>] [-Out <build dir>] [-Clean]
# Output: <NX>\build\<checkout name>\<id>\release (NX\build\main\mpr\release; a worktree NX\wt\x builds into NX\build\x\mpr), with -Release ...\relpkg
# (NXRT_RELEASE=ON: GUI program, ./exefs, no build paths in the files): the folder tools\release\make_release.py packs.
# -LibDir: the lifted libraries (default: games\<id>\work\build\<id> of the main checkout). -A2B: a built astc2bc7, tools\textures (default NXRT_A2B_DIR, else
# NX\build\textures). The game is built against the base this script is in; its data (game\, work\) is read from the main checkout. Exact commands: docs\BUILD.md.
param([string]$Game = $env:NX_GAME_DIR, [string]$Config = "Release", [switch]$Clean, [switch]$Release, [string]$LibDir = "", [string]$Out = "",
      [string]$Version = "dev", [string]$A2B = $env:NXRT_A2B_DIR)
$ErrorActionPreference = "Stop"
if (-not $Game) { throw "pass -Game <game folder> (or set NX_GAME_DIR)" }
$Game = [IO.Path]::GetFullPath($Game)
if (-not (Test-Path (Join-Path $Game "game.json"))) { throw "no game manifest $Game\game.json" }
$id = (Get-Content -Raw (Join-Path $Game "game.json") | ConvertFrom-Json).id
$base = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $base "scripts\nxpaths.ps1")
# game data (work\, game\) exists only in the main checkout (or in a game folder that holds its own work\)
$data = if (Test-Path (Join-Path $Game "work")) { $Game } else { Join-Path $env:NX_ROOT "games\$id" }
if (-not $LibDir) { $LibDir = Join-Path $data "work\build\$id" }
$bd = if ($Out) { $Out } else { Join-Path $env:NX_ROOT "build\$($env:NX_CHECKOUT_NAME)\$id" }
$bd = if ($Release) { Join-Path $bd "relpkg" } else { Join-Path $bd $Config.ToLower() }
if ($Clean -and (Test-Path $bd)) { Remove-Item -Recurse -Force $bd }
$src = Join-Path $PSScriptRoot "..\cmake\game_project"
$cfg = "cmake -S `"$src`" -B `"$bd`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache -DNX_GAME_DIR=`"$Game`" -DGAME_VERSION=$Version"
if ($A2B) { $cfg += " -DNXRT_A2B_DIR=`"$A2B`"" }
if ($LibDir) { $cfg += " -DGAME_LIB_DIR=`"$([IO.Path]::GetFullPath($LibDir))`"" }
$targets = $id
if ($Release) { $cfg += " -DNXRT_RELEASE=ON"; $targets = "$id ${id}_launcher" }
cmd /c "$cfg && cmake --build `"$bd`" --target $targets"
exit $LASTEXITCODE
