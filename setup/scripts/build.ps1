# Configure + build the setup tool (library, CLI, GUI, tests) with CMake + Ninja + clang-cl, then optionally run the tests.
#   pwsh -File tools\setup\scripts\build.ps1 [-Clean] [-Test] [-Target name] [-BuildDir path] [-Version 0.1.0]
# Needs tools\textures\third_party (fetch_third_party.ps1 there). Output: <BuildDir>\bin\ (default tools\setup\build\release)
param([switch]$Clean, [switch]$Test, [string]$Target = '', [string]$BuildDir = '', [string]$Version = 'dev')
$ErrorActionPreference = 'Stop'
$root  = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = if ($BuildDir) { $BuildDir } else { Join-Path $root 'build\release' }
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
foreach ($line in (cmd /c "`"$vcvars`" >nul 2>&1 && set")) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { Set-Item -Path ("Env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
}
$env:PATH = "C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;$env:LOCALAPPDATA\Microsoft\WinGet\Links;$env:PATH"
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DMPRS_VERSION="$Version"
if ($LASTEXITCODE) { throw 'cmake configure failed' }
if ($Target) { cmake --build $build --target $Target } else { cmake --build $build }
if ($LASTEXITCODE) { throw 'build failed' }
if ($Test) {
    ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE) { throw 'tests failed' }
}
Write-Output "built: $build\bin"
