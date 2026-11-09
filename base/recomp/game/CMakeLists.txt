# SPDX-License-Identifier: GPL-3.0-or-later
# The lifted libraries of one game: the CMake project scripts/build_game.cmd configures, with the game folder as its input.
#   cmake -S recomp/game -B <game dir>/work/build/<id> -DNX_GAME_DIR=<game dir> (toolchain cmake/clang-cl.cmake)
# Targets (cmake/NxRecomp.cmake nxrecomp_add_game): <id>_<module> for each module of the manifest, <id>_modules, <id>_link_check.
cmake_minimum_required(VERSION 3.25)
project(nxrecomp_game C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NxRecomp.cmake")
set(NX_GAME_DIR "" CACHE PATH "the game folder (games/<id>: game.json, recomp/, game/exefs)")
if(NOT NX_GAME_DIR)
    message(FATAL_ERROR "set NX_GAME_DIR to the game folder (games/<id>)")
endif()
set(NX_GAME_EXEFS "" CACHE PATH "folder with the game's NSOs (default <game dir>/game/exefs)")
set(NX_GAME_GEN "" CACHE PATH "where the generated sources go (default <game dir>/work/lift)")
nxrecomp_add_game(DIR "${NX_GAME_DIR}" EXEFS "${NX_GAME_EXEFS}" GEN "${NX_GAME_GEN}")
