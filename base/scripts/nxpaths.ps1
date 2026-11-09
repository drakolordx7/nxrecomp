# Dot-source it from a base PowerShell script: runs scripts\env.cmd once and takes its environment (MSVC, LLVM, CMake, Ninja, NX_ROOT,
# NX_BASE, NX_CHECKOUT, NX_CHECKOUT_NAME, NX_PYTHON, ...) into this process.
#   . (Join-Path $PSScriptRoot '..\..\scripts\nxpaths.ps1')     then $env:NX_ROOT is the main checkout (games\, build\, wt\), $env:NX_CHECKOUT the
#   checkout of this script (the main one or wt\<name>) and $env:NX_CHECKOUT_NAME 'main' or <name>: the folder build\<name> holds its output
foreach ($line in (cmd /c "call `"$PSScriptRoot\env.cmd`" && set")) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { Set-Item -Path ("Env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
}
