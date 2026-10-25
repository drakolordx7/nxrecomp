# SPDX-License-Identifier: GPL-3.0-or-later
#
# Metroid Prime Remastered (v1.0.0): lift the NSOs of the user's dump and build one static library per module.
#
#   scripts\build_mpr.cmd                         (configure + build into work\build\mpr; MPR_ROOT is the repository root)
#
# Inputs (read-only): <MPR_ROOT>\game\exefs\{main,sdk,subsdk0,rtld}.   Generated C++ goes to <MPR_ROOT>\work\lift\<module>,
# build output to work\build\mpr: neither is ever committed. Targets: mpr_main, mpr_sdk, mpr_subsdk0, mpr_rtld (static libraries,
# each provides `nxmod_<name>`: ctx.h NxModule), and `mpr_modules` builds all three.
cmake_minimum_required(VERSION 3.25)
project(mpr_lift C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NxRecomp.cmake")

get_filename_component(MPR_ROOT_DEFAULT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(DEFINED NX_ROOT)   # scripts/build_game.cmd passes the repository root as NX_ROOT
    set(MPR_ROOT_DEFAULT "${NX_ROOT}")
endif()
set(MPR_ROOT "${MPR_ROOT_DEFAULT}" CACHE PATH "repository root (game/, work/)")
set(MPR_EXEFS "${MPR_ROOT}/game/exefs" CACHE PATH "folder with the NSOs main, sdk, subsdk0 (docs/S1b.md)")
set(MPR_GEN "${MPR_ROOT}/work/lift" CACHE PATH "where the generated sources go")
# translation units per module: a unit of ~45k instructions compiles in ~10 s with ~0.4 GB (docs/S1b.md)
set(MPR_SHARDS_MAIN 96 CACHE STRING "translation units of main")
set(MPR_SHARDS_SDK 32 CACHE STRING "translation units of sdk")
set(MPR_SHARDS_SUBSDK0 20 CACHE STRING "translation units of subsdk0")

# module order and bases = recomp/games/mpr/config.py (runtime/include/nxrt/memory.h: main 0x7100000000, SDK modules from 0x7500000000)
nxrecomp_add_module(mpr_main INPUT "${MPR_EXEFS}/main" NAME main INDEX 0 BASE 0x7100000000 GAME mpr SHARDS ${MPR_SHARDS_MAIN}
                    SITES "${CMAKE_CURRENT_LIST_DIR}/main.sites" OUTDIR "${MPR_GEN}/main")
nxrecomp_add_module(mpr_sdk INPUT "${MPR_EXEFS}/sdk" NAME sdk INDEX 1 BASE 0x7500000000 GAME mpr SHARDS ${MPR_SHARDS_SDK}
                    OUTDIR "${MPR_GEN}/sdk")
nxrecomp_add_module(mpr_subsdk0 INPUT "${MPR_EXEFS}/subsdk0" NAME subsdk0 INDEX 2 BASE 0x7600000000 GAME mpr SHARDS ${MPR_SHARDS_SUBSDK0}
                    OUTDIR "${MPR_GEN}/subsdk0")
# rtld (7 KB, the process entry: relocates the other modules, calls nn::init::Start) is lifted too, as the runtime may prefer it
# to an HLE loader; index 3, base 0x7700000000 (below the heap view at 0x7800000000)
nxrecomp_add_module(mpr_rtld INPUT "${MPR_EXEFS}/rtld" NAME rtld INDEX 3 BASE 0x7700000000 GAME mpr SHARDS 1
                    OUTDIR "${MPR_GEN}/rtld")
add_custom_target(mpr_modules DEPENDS mpr_main mpr_sdk mpr_subsdk0 mpr_rtld)

# Link check (target mpr_link_check): the three libraries plus stubs for what the runtime provides. Proves every symbol resolves.
add_executable(mpr_link_check EXCLUDE_FROM_ALL link_check.cpp)
target_link_libraries(mpr_link_check PRIVATE mpr_main mpr_sdk mpr_subsdk0 mpr_rtld)
target_compile_options(mpr_link_check PRIVATE /w /EHs-c- /GR-)
