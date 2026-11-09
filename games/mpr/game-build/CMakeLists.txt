# SPDX-License-Identifier: GPL-3.0-or-later
# mpr.exe and mpr_launcher.exe: the base runtime + this game's runtime module (../runtime) + the lifted game libraries (built by the base's
# recomp/scripts/build_game.cmd <this game folder> into ../work/build/mpr). Names, ids and modules come from ../game.json.
#   pwsh game-build\scripts\build.ps1 [-Release] [-Version 0.1.0]      exact commands: base docs/BUILD.md
# Nothing derived from the game lives here: the libraries are imported from the work folder, the NSOs are read at run time.
cmake_minimum_required(VERSION 3.25)
# Static MSVC runtime (/MT): players need no Visual C++ Redistributable. Set before project() so every target below, third-party code
# included, uses it; the libraries this one links must agree (a mismatch is a link error).
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
project(mpr_game C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
# NX_BASE: the base checkout. Default: ./base inside the game folder when it is populated (a public clone with the base as a submodule),
# else ../../base (the NX layout: NX/games/<id> next to NX/base).
if(NOT NX_BASE)
    if(EXISTS "${GAME_DIR}/base/runtime/CMakeLists.txt")
        set(_nx_base "${GAME_DIR}/base")
    else()
        get_filename_component(_nx_base "${GAME_DIR}/../../base" ABSOLUTE)
    endif()
    set(NX_BASE "${_nx_base}" CACHE PATH "the base checkout (runtime/, recomp/, tools/)")
endif()
if(NOT EXISTS "${NX_BASE}/runtime/CMakeLists.txt")
    message(FATAL_ERROR "NX_BASE=${NX_BASE} is not a base checkout (pass -DNX_BASE=<base>)")
endif()
set(GAME_LIB_DIR "${GAME_DIR}/work/build/mpr" CACHE PATH "folder with the lifted libraries mpr_<module>.lib (base recomp/scripts/build_game.cmd)")
# The release version ("0.1.0"; "dev" for local builds): the game log, --version, the launcher window and the file properties show it.
set(GAME_VERSION "dev" CACHE STRING "release version shown by mpr.exe and the launcher")

set(NXRT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NXRT_GAME_DIRS "${GAME_DIR}" CACHE STRING "" FORCE)
add_subdirectory("${NX_BASE}/runtime" runtime)
nxrt_add_game_exe(mpr GAME_DIR "${GAME_DIR}" LIB_DIR "${GAME_LIB_DIR}" VERSION "${GAME_VERSION}")
