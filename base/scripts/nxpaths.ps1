# Dot-source it from a base PowerShell script: runs scripts\env.cmd once and takes its environment (MSVC, LLVM, CMake, Ninja, NX_ROOT,
# NX_BASE, NX_PYTHON, ...) into this process.
#   . (Join-Path $PSScriptRoot '..\..\scripts\nxpaths.ps1')     then $env:NX_ROOT is the folder with games\ and build\ (may be empty)
foreach ($line in (cmd /c "call `"$PSScriptRoot\env.cmd`" && set")) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { Set-Item -Path ("Env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
}
