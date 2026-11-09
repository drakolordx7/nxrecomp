# Make a git worktree of this base checkout on a new branch and configure its runtime build (graphics shell off: fast, no network).
#   pwsh runtime\scripts\wt_setup.ps1 svc-time          -> <NX>\wt\base-svc-time on branch svc-time, build <NX>\build\base-svc-time\runtime\release
param([Parameter(Mandatory = $true)][string]$Name)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "proj.ps1")
if (-not $nx) { throw "no NX folder above $base (set NX_ROOT)" }
$dir = Join-Path $nx "wt\base-$Name"
git -C $base worktree add -b $Name $dir HEAD
$bd = Join-Path $nx "build\base-$Name\runtime\release"
$cfg = "cmake -S `"$dir\runtime`" -B `"$bd`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache -DNXRT_BUILD_GFX=OFF -DNXRT_BUILD_LIFT_TESTS=OFF"
cmd /c "call `"$(Join-Path $dir 'runtime\scripts\env.cmd')`" && $cfg"
exit $LASTEXITCODE
