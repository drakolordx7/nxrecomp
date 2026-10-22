@echo off
rem Metroid Prime Remastered: configure and build the three module libraries (work\build\mpr), sccache kept in work\build\sccache.
rem   scripts\build_mpr.cmd [extra ninja args, e.g. -j 20 | -k 0 | mpr_sdk]
rem Environment: MPR_JOBS (default 20) = compile processes; MPR_BUILD (default <root>\work\build\mpr).
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
if not defined MPR_BUILD set "MPR_BUILD=%ROOT%\work\build\mpr"
if not defined MPR_JOBS set "MPR_JOBS=20"
if not defined SCCACHE_DIR set "SCCACHE_DIR=%ROOT%\work\build\sccache"
if not defined SCCACHE_SERVER_PORT set "SCCACHE_SERVER_PORT=24339"
if not defined SCCACHE_CACHE_SIZE set "SCCACHE_CACHE_SIZE=20G"
call "%HERE%nx_env.cmd" cmake -S "%HERE%..\games\mpr" -B "%MPR_BUILD%" -G Ninja -DCMAKE_TOOLCHAIN_FILE="%HERE%..\cmake\clang-cl.cmake" -DCMAKE_BUILD_TYPE=Release -DMPR_ROOT="%ROOT%"
if errorlevel 1 exit /b 1
call "%HERE%nx_env.cmd" cmake --build "%MPR_BUILD%" --target mpr_modules -- -j %MPR_JOBS% %*
exit /b %errorlevel%
