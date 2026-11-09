# Dot-sourced by the runtime scripts: the folders around this checkout (base docs/BUILD.md).
#   $base     the base folder (holds runtime\, recomp\, tools\): <checkout>\base
#   $nx       the main checkout (the local-only game data, build\, wt\): $env:NX_ROOT, else the parent of wt\ when this checkout is a worktree, else the checkout
#   $name     this checkout's build folder name: 'main' for the main checkout, else the worktree's folder name ($env:NX_CHECKOUT_NAME)
#   $recomp   the recompiler's include folder: $env:NXRT_RECOMP_INCLUDE, else $base\recomp\include
#   $rtbuild  the runtime build folder for $Config (default Release): $env:NXRT_BUILD_DIR, else $nx\build\<name>\runtime\<config>
$base = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $base "scripts\nxpaths.ps1")
$nx = $env:NX_ROOT   # scripts\nxpaths.ps1 (scripts\env.cmd) sets NX_ROOT and NX_CHECKOUT_NAME
$name = $env:NX_CHECKOUT_NAME
$recomp = if ($env:NXRT_RECOMP_INCLUDE) { $env:NXRT_RECOMP_INCLUDE } else { Join-Path $base "recomp\include" }
$_cfg = if ($Config) { $Config.ToLower() } else { "release" }
$rtbuild = if ($env:NXRT_BUILD_DIR) { $env:NXRT_BUILD_DIR } else { Join-Path $nx "build\$name\runtime\$_cfg" }
