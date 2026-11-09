# SPDX-License-Identifier: GPL-3.0-or-later
# One game's exe and launcher: the base runtime + the game's runtime module (<game dir>/runtime) + the lifted libraries (built by
# recomp/scripts/build_game.cmd into <game dir>/work/build/<id>). Names, ids and modules come from <game dir>/game.json.
#   pwsh runtime\scripts\build_game_exe.ps1 -Game <games\<id>> [-Release] [-Version 0.1.0]      exact commands: docs/BUILD.md
# Nothing derived from the game lives here: the libraries are imported from the work folder, the NSOs are read at run time.
cmake_minimum_required(VERSION 3.25)
# Static MSVC runtime (/MT): players need no Visual C++ Redistributable. Set before project() so every target below, third-party code
# included, uses it; the libraries this one links must agree (a mismatch is a link error).
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
project(nx_game_exe C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(NX_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${NX_BASE_DIR}/cmake/nx_game.cmake")
set(NX_GAME_DIR "" CACHE PATH "the game folder (games/<id>: game.json, runtime/)")
if(NOT NX_GAME_DIR)
    message(FATAL_ERROR "set NX_GAME_DIR to the game folder (games/<id>)")
endif()
nx_game_read("${NX_GAME_DIR}" G)
set(GAME_LIB_DIR "${G_DIR}/work/build/${G_ID}" CACHE PATH "folder with the lifted libraries <id>_<module>.lib (recomp/scripts/build_game.cmd)")
# The release version ("0.1.0"; "dev" for local builds): the game log, --version, the launcher window and the file properties show it.
set(GAME_VERSION "dev" CACHE STRING "release version shown by the game exe and the launcher")

set(NXRT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NXRT_GAME_DIRS "${G_DIR}" CACHE STRING "" FORCE)
add_subdirectory("${NX_BASE_DIR}/runtime" runtime)
nxrt_add_game_exe(${G_ID} GAME_DIR "${G_DIR}" LIB_DIR "${GAME_LIB_DIR}" VERSION "${GAME_VERSION}")
