# Builds mx_batch.exe (the batch Maxwell -> GLSL -> SPIR-V driver) with clang-cl, into tools\shaders\build\smo\.
#   pwsh -File tools\shaders\smo\build.ps1
# Needs LLVM and the VS Build Tools (scripts\env.cmd of the base finds both).
$ErrorActionPreference = 'Stop'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$out   = Join-Path $here '..\build\smo' | ForEach-Object { [System.IO.Path]::GetFullPath($_) }
$obj   = Join-Path $out 'obj'
. (Join-Path $here '..\..\..\scripts\nxpaths.ps1')

New-Item -ItemType Directory -Force -Path $obj | Out-Null
$src = @((Join-Path $here 'mx_batch.c'), (Join-Path $here 'mx_translate.c'))
$exe = Join-Path $out 'mx_batch.exe'

# _CRT_SECURE_NO_WARNINGS: mx_translate.c uses getenv/strcpy, which MSVC's CRT marks deprecated; the code is unchanged.
$fe = '/Fe' + $exe
$fo = '/Fo' + $obj + '\'
& clang-cl /nologo /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS "/I$here" @src $fe $fo 2>&1 | ForEach-Object { $_ }
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
Write-Output "built: $exe"
