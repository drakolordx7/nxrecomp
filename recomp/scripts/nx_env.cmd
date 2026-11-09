@echo off
rem Sets up the build environment: MSVC x64 (vcvars64), LLVM clang-cl, CMake, Ninja, sccache, Python.
rem Usage from cmd:  scripts\nx_env.cmd cmake --build build\x
rem                  scripts\nx_env.cmd            (no arguments: just prints the tool versions)
rem Tool locations (the shell side of cmake/nx_toolchain.cmake; set any of these beforehand to override):
rem   Visual Studio   the newest install with the x64 C++ tools, found with vswhere (skipped when vcvars has already run)
rem   LLVM_BIN        default %ProgramFiles%\LLVM\bin
rem   NX_PYTHON       default <base>\tools\.venv\Scripts\python.exe if it exists, else python on PATH
if defined VSCMD_ARG_TGT_ARCH goto :vs_done
set "NX_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%I in (`call "%NX_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "NX_VS=%%I"
if defined NX_VS call "%NX_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
:vs_done
if not defined LLVM_BIN set "LLVM_BIN=%ProgramFiles%\LLVM\bin"
if not defined NX_PYTHON if exist "%~dp0..\..\tools\.venv\Scripts\python.exe" set "NX_PYTHON=%~dp0..\..\tools\.venv\Scripts\python.exe"
set "PATH=%LLVM_BIN%;%ProgramFiles%\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
if defined NX_PYTHON for %%P in ("%NX_PYTHON%") do set "PATH=%%~dpP;%PATH%"
if "%~1"=="" (
  clang-cl --version & cmake --version & ninja --version & sccache --version
  exit /b 0
)
%*
