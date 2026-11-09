@echo off
rem The one build environment of every base script: MSVC x64 (vcvars64), LLVM, CMake, Ninja, sccache, Python, and the NX folders.
rem   call scripts\env.cmd                  sets the environment in the calling cmd (silent)
rem   scripts\env.cmd <command> [args]      runs the command in it
rem   scripts\env.cmd --versions            prints the tool versions
rem Set any of these beforehand to override:
rem   NX_ROOT     the main checkout (holds the local-only games\<id>\game and work, build\, refs\, wt\): default the parent of wt\ when
rem               this checkout is a worktree (NX\wt\<name>), else this checkout
rem   NX_BASE     the base folder, <checkout>\base: default the one holding this script
rem Set by this script: NX_CHECKOUT (the parent of NX_BASE: the main checkout or a worktree) and NX_CHECKOUT_NAME ("main", else the worktree's
rem folder name): build output goes to NX_ROOT\build\<NX_CHECKOUT_NAME>\.
rem   VS          skipped when vcvars has already run; else the newest install with the x64 C++ tools, found with vswhere
rem   LLVM_BIN    default %ProgramFiles%\LLVM\bin
rem   NX_PYTHON   default <base>\tools\.venv\Scripts\python.exe, else <NX_ROOT>\base\tools\.venv (the main checkout's), else python on PATH
rem   NX_JOBS     compile processes, default NUMBER_OF_PROCESSORS
if not defined NX_BASE for %%I in ("%~dp0..") do set "NX_BASE=%%~fI"
for %%I in ("%NX_BASE%\..") do set "NX_CHECKOUT=%%~fI"
if not defined NX_ROOT call :find_root "%NX_CHECKOUT%"
if /i "%NX_CHECKOUT%"=="%NX_ROOT%" (set "NX_CHECKOUT_NAME=main") else for %%I in ("%NX_CHECKOUT%") do set "NX_CHECKOUT_NAME=%%~nxI"
if defined VSCMD_ARG_TGT_ARCH goto :vs_done
set "NX_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%I in (`call "%NX_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "NX_VS=%%I"
if defined NX_VS call "%NX_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
:vs_done
if not defined LLVM_BIN set "LLVM_BIN=%ProgramFiles%\LLVM\bin"
if not defined NX_PYTHON if exist "%NX_BASE%\tools\.venv\Scripts\python.exe" set "NX_PYTHON=%NX_BASE%\tools\.venv\Scripts\python.exe"
if not defined NX_PYTHON if exist "%NX_ROOT%\base\tools\.venv\Scripts\python.exe" set "NX_PYTHON=%NX_ROOT%\base\tools\.venv\Scripts\python.exe"
if not defined NX_JOBS set "NX_JOBS=%NUMBER_OF_PROCESSORS%"
set "PATH=%LLVM_BIN%;%ProgramFiles%\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
if defined NX_PYTHON for %%P in ("%NX_PYTHON%") do set "PATH=%%~dpP;%PATH%"
if "%~1"=="" exit /b 0
if /i "%~1"=="--versions" (
  clang-cl --version & cmake --version & ninja --version & sccache --version
  exit /b 0
)
%*
exit /b %errorlevel%

:find_root
set "NX_ROOT=%~f1"
for %%I in ("%~f1\..") do (set "NX_UP=%%~fI" & set "NX_UPN=%%~nxI")
if /i "%NX_UPN%"=="wt" for %%I in ("%NX_UP%\..") do set "NX_ROOT=%%~fI"
exit /b 0
