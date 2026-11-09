@echo off
rem Configure and build the lifted module libraries of one game, sccache kept in <game dir>\work\build\sccache.
rem   scripts\build_game.cmd <game dir> [extra ninja args, e.g. -j 20 | -k 0 | <id>_<module> | <id>_link_check]
rem The game dir (games\<id>) holds the manifest game.json, recomp\config.py and the sites, game\exefs (the dump's NSOs); the project is
rem recomp\game (cmake/NxRecomp.cmake nxrecomp_add_game), with the targets <id>_<module>, <id>_modules and <id>_link_check.
rem Environment (scripts\env.cmd of the base): NX_JOBS = compile processes; NX_BUILD (default <game dir>\work\build\<id>);
rem NX_SCCACHE_PORT (default 24339) = the sccache server of this build.
setlocal
if "%~1"=="" (
    echo usage: scripts\build_game.cmd ^<game dir^> [ninja args]
    exit /b 2
)
rem the game folder without quotes, and the arguments after it (cmd's shift does not change %*)
set "GAMEDIR=%~f1"
set "ARGS=%*"
call set "ARGS=%%ARGS:*%1=%%"
set "HERE=%~dp0"
if not exist "%GAMEDIR%\game.json" (
    echo no game manifest %GAMEDIR%\game.json
    exit /b 2
)
call "%HERE%..\..\scripts\env.cmd"
rem the game id is the manifest's (a worktree's folder is <id>-<topic>)
for /f "usebackq delims=" %%I in (`python -c "import json,sys;print(json.load(open(sys.argv[1],encoding='utf-8'))['id'])" "%GAMEDIR%\game.json"`) do set "GAME=%%I"
if not defined GAME (echo cannot read the id of %GAMEDIR%\game.json & exit /b 2)
if not defined NX_BUILD set "NX_BUILD=%GAMEDIR%\work\build\%GAME%"
if not defined SCCACHE_DIR set "SCCACHE_DIR=%GAMEDIR%\work\build\sccache"
if not defined SCCACHE_SERVER_PORT set "SCCACHE_SERVER_PORT=%NX_SCCACHE_PORT%"
if not defined SCCACHE_SERVER_PORT set "SCCACHE_SERVER_PORT=24339"
if not defined SCCACHE_CACHE_SIZE set "SCCACHE_CACHE_SIZE=20G"
cmake -S "%HERE%..\game" -B "%NX_BUILD%" -G Ninja -DCMAKE_TOOLCHAIN_FILE="%HERE%..\cmake\clang-cl.cmake" -DCMAKE_BUILD_TYPE=Release -DNX_GAME_DIR="%GAMEDIR%"
if errorlevel 1 exit /b 1
cmake --build "%NX_BUILD%" --target %GAME%_modules -- -j %NX_JOBS%%ARGS%
exit /b %errorlevel%
