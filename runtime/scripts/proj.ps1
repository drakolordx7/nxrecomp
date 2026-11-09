# Dot-sourced by the runtime scripts: the folders around this checkout (base docs/BUILD.md).
#   $base     the base checkout (holds runtime\, recomp\, tools\)
#   $nx       the NX folder (holds games\ and build\): $env:NX_ROOT, else the nearest parent of the checkout with a games\ folder (may be empty)
#   $recomp   the recompiler's include folder: $env:NXRT_RECOMP_INCLUDE, else $base\recomp\include
#   $rtbuild  the runtime build folder for $Config (default Release): $env:NXRT_BUILD_DIR, else $nx\build\<checkout name>\runtime\<config>,
#             else (outside an NX folder) <runtime>\build\<config>
$base = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$nx = $env:NX_ROOT
if (-not $nx) {
    $d = Split-Path -Parent $base
    while ($d -and -not (Test-Path (Join-Path $d "games"))) { $d = Split-Path -Parent $d }
    $nx = $d
}
$recomp = if ($env:NXRT_RECOMP_INCLUDE) { $env:NXRT_RECOMP_INCLUDE } else { Join-Path $base "recomp\include" }
$_cfg = if ($Config) { $Config.ToLower() } else { "release" }
$rtbuild = if ($env:NXRT_BUILD_DIR) { $env:NXRT_BUILD_DIR }
           elseif ($nx) { Join-Path $nx ("build\" + (Split-Path -Leaf $base) + "\runtime\" + $_cfg) }
           else { Join-Path $base ("runtime\build\" + $_cfg) }
