# Configure + build mpr.exe (and mpr_launcher.exe) from this game folder: the base runtime is compiled in, the lifted libraries are imported.
#   pwsh game-build\scripts\build.ps1 [-Release] [-Version 0.1.0] [-Base <base checkout>] [-LibDir <dir>] [-Out <build dir>] [-Clean]
# Output: <NX>\build\<this checkout's folder name> (e.g. NX\build\mpr; a worktree NX\wt\mpr-x builds into NX\build\mpr-x), with -Release
# ...\relpkg (NXRT_RELEASE=ON: GUI program, ./exefs, no build paths in the files): the folder base tools\release\make_release.py packs.
# -Base: the base checkout (default NX_BASE, else .\base when populated, else ..\..\base). -LibDir: the lifted libraries (default
# work\build\mpr). -A2B: a built astc2bc7, base tools\textures (default NXRT_A2B_DIR, else NX\build\textures). Exact commands: base
# docs\BUILD.md.
param([string]$Config = "Release", [switch]$Clean, [switch]$Release, [string]$Base = $env:NX_BASE, [string]$LibDir = "", [string]$Out = "",
      [string]$Version = "dev", [string]$A2B = $env:NXRT_A2B_DIR)
$ErrorActionPreference = "Stop"
$src = Split-Path -Parent $PSScriptRoot
$game = Split-Path -Parent $src
if (-not $Base) { $Base = if (Test-Path (Join-Path $game "base\runtime\CMakeLists.txt")) { Join-Path $game "base" } else { Join-Path $game "..\..\base" } }
$Base = [IO.Path]::GetFullPath($Base)
$nx = $env:NX_ROOT
if (-not $nx) { $d = Split-Path -Parent $game; while ($d -and -not (Test-Path (Join-Path $d "games"))) { $d = Split-Path -Parent $d }; $nx = $d }
$name = Split-Path -Leaf $game
$bd = if ($Out) { $Out } elseif ($nx) { Join-Path $nx ("build\" + $name) } else { Join-Path $src "build" }
$bd = if ($Release) { Join-Path $bd "relpkg" } else { Join-Path $bd $Config.ToLower() }
if ($Clean -and (Test-Path $bd)) { Remove-Item -Recurse -Force $bd }
$env_cmd = Join-Path $Base "runtime\scripts\env.cmd"
$cfg = "cmake -S `"$src`" -B `"$bd`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache -DNX_BASE=`"$Base`" -DGAME_VERSION=$Version"
if ($A2B) { $cfg += " -DNXRT_A2B_DIR=`"$A2B`"" }
if ($LibDir) { $cfg += " -DGAME_LIB_DIR=`"$([IO.Path]::GetFullPath($LibDir))`"" }
$targets = "mpr"
if ($Release) { $cfg += " -DNXRT_RELEASE=ON"; $targets = "mpr mpr_launcher" }
cmd /c "call `"$env_cmd`" && $cfg && cmake --build `"$bd`" --target $targets"
exit $LASTEXITCODE
