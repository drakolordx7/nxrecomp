# Dot-sourced by the runtime scripts: the folders around this checkout (base docs/BUILD.md).
#   $base     the base checkout (holds runtime\, recomp\, tools\)
#   $nx       the NX folder (holds games\ and build\): $env:NX_ROOT, else the nearest parent of the checkout with a games\ folder (may be empty)
#   $recomp   the recompiler's include folder: $env:NXRT_RECOMP_INCLUDE, else $base\recomp\include
#   $rtbuild  the runtime build folder for $Config (default Release): $env:NXRT_BUILD_DIR, else $nx\build\<checkout name>\runtime\<config>,
#             else (outside an NX folder) <runtime>\build\<config>
$base = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $base "scripts\nxpaths.ps1")
$nx = $env:NX_ROOT   # scripts\nxpaths.ps1 sets it: env NX_ROOT, else the nearest parent of the checkout with a games\ folder
$recomp = if ($env:NXRT_RECOMP_INCLUDE) { $env:NXRT_RECOMP_INCLUDE } else { Join-Path $base "recomp\include" }
$_cfg = if ($Config) { $Config.ToLower() } else { "release" }
$rtbuild = if ($env:NXRT_BUILD_DIR) { $env:NXRT_BUILD_DIR }
           elseif ($nx) { Join-Path $nx ("build\" + (Split-Path -Leaf $base) + "\runtime\" + $_cfg) }
           else { Join-Path $base ("runtime\build\" + $_cfg) }
