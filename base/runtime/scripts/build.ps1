# Configure + build (+ test) the base runtime alone (no game). Run from anywhere:
#   pwsh scripts\build.ps1 [-Test] [-Config Release] [-Clean] [-Filter <regex>] [-A2B <astc2bc7 build dir>] [-Games <dir;dir>]
# Output: $rtbuild (scripts\proj.ps1: <NX>\build\<checkout name>\runtime\<config>), so worktrees build on their own. -Games lists game folders
# whose runtime modules (and tests) are built too (NXRT_GAME_DIRS). Exact commands: docs\BUILD.md.
param([string]$Config = "Release", [switch]$Test, [switch]$Clean, [string]$Filter = "", [switch]$Verbose, [string]$A2B = "", [string]$Games = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "proj.ps1")
$bd = $rtbuild
if ($Clean -and (Test-Path $bd)) { Remove-Item -Recurse -Force $bd }
$env_cmd = Join-Path $PSScriptRoot "env.cmd"
$cfg = "cmake -S `"$root`" -B `"$bd`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache"
if ($A2B) { $cfg += " -DNXRT_A2B_DIR=`"$A2B`"" }
if ($Games) { $cfg += " -DNXRT_GAME_DIRS=`"$Games`"" }
$bld = "cmake --build `"$bd`""
if ($Verbose) { $bld += " --verbose" }
$cmd = "call `"$env_cmd`" && $cfg && $bld"
if ($Test) { $cmd += " && ctest --test-dir `"$bd`" --output-on-failure"; if ($Filter) { $cmd += " -R $Filter" } }
cmd /c $cmd
exit $LASTEXITCODE
