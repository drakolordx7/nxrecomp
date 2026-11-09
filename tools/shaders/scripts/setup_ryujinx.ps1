# Stage the three Ryujinx projects the translator needs into tools\shaders\third_party\ryubing (git-ignored).
# Source: the read-only checkout refs\ryubing (Ryubing fork, Codeberg archive a82350bb77, MIT). The build writes obj/bin
# next to the sources, so we build from this copy and never touch refs\.
param([string]$Ryubing = '')   # default <NX>\refs\ryubing
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\..\scripts\nxpaths.ps1')
if (-not $Ryubing) {
    if (-not $env:NX_ROOT) { throw 'no NX folder around this checkout: pass -Ryubing <the ryubing checkout> or set NX_ROOT' }
    $Ryubing = Join-Path $env:NX_ROOT 'refs\ryubing'
}
$dst = Join-Path $PSScriptRoot '..\third_party\ryubing'
New-Item -ItemType Directory -Force "$dst\src" | Out-Null
foreach ($p in 'Ryujinx.Common','Spv.Generator','Ryujinx.Graphics.Shader') {
    robocopy "$Ryubing\src\$p" "$dst\src\$p" /MIR /XD obj bin /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy $p failed ($LASTEXITCODE)" }
}
foreach ($f in 'Directory.Build.props','Directory.Packages.props','global.json','nuget.config','LICENSE.txt') {
    Copy-Item "$Ryubing\$f" "$dst\$f" -Force
}
Write-Host "staged Ryujinx sources in $dst"
$global:LASTEXITCODE = 0
