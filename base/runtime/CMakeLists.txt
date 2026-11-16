# SPDX-License-Identifier: GPL-3.0-or-later
# nxrt: the game-independent runtime (HLE kernel and services, NVN on Vulkan, play mode, launcher) of the NX static recompilations.
# Build, Windows x64: scripts\build.ps1   (clang-cl + Ninja, Release)   Test: ctest --test-dir build\release
# Build, Linux x86-64: cmake --preset linux-release -B <dir>   (clang + lld + Ninja; presets in CMakePresets.json, toolchain cmake/linux-clang.cmake)
# Which platform and what differs (flags, library names, src/os/<platform>): ../cmake/nx_platform.cmake.
cmake_minimum_required(VERSION 3.25)
# The platform (before project(): the static MSVC runtime is set there on Windows)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/nx_platform.cmake")
project(nxrt C CXX)
nx_platform_setup()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

option(NXRT_BUILD_TESTS "Build the nxrt_tests executable and register its CTest entries" ON)

# The base checkout (base/: recomp, runtime, tools in one repository) and the NX layout around it (cmake/nx_game.cmake: NX_ROOT, game manifests).
get_filename_component(NX_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
include("${NX_BASE_DIR}/cmake/nx_game.cmake")
# The generated-code ABI header (Ctx, call_indirect, ...) lives in the recompiler of the same checkout (recomp/include/nxrecomp/).
set(NXRT_RECOMP_INCLUDE "${NX_BASE_DIR}/recomp/include" CACHE PATH "Directory that holds nxrecomp/")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(${NX_RT_COMPILE_OPTIONS})
endif()
if(NX_RT_COMPILE_DEFINITIONS)
    add_compile_definitions(${NX_RT_COMPILE_DEFINITIONS})
endif()

option(NXRT_BUILD_GFX "Build the graphics shell (SDL3 + Vulkan + NVN front end, src/gfx, cmake/gfx.cmake)" ON)
# Game folders (games/<id>/, each with game.json and runtime/CMakeLists.txt; docs/GAME_BOUNDARY.md): the title-specific modules a game exe links.
# Empty (the default) is the neutral base build. Each game's own tests (games/<id>/runtime/tests) are built with it when tests are on.
set(NXRT_GAME_DIRS "" CACHE STRING "game folders to build the runtime modules of (absolute paths, a list)")
# Package builds (game exes as release binaries): no local source paths in the binaries (-ffile-prefix-map of the base and game folders).
option(NXRT_RELEASE "build for a release package (no build paths in the binaries)" OFF)
# __FILE__ (the assert strings of the audio renderer) never carries the build folder, in any build: paths are written relative to the checkout.
nx_platform_clang_option(_nx_mpm "-fmacro-prefix-map=${NX_BASE_DIR}=.")
add_compile_options("${_nx_mpm}")
foreach(_gd ${NXRT_GAME_DIRS})
    nx_platform_clang_option(_nx_mpm "-fmacro-prefix-map=${_gd}=.")
    add_compile_options("${_nx_mpm}")
endforeach()
if(NXRT_RELEASE)
    nx_platform_clang_option(_nx_fpm "-ffile-prefix-map=${NX_BASE_DIR}=.")
    add_compile_options("${_nx_fpm}")
    foreach(_gd ${NXRT_GAME_DIRS})
        nx_platform_clang_option(_nx_fpm "-ffile-prefix-map=${_gd}=.")
        add_compile_options("${_nx_fpm}")
    endforeach()
endif()

file(GLOB_RECURSE NXRT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/gfx/")   # the graphics shell is its own library (cmake/gfx.cmake)
# The platform layer: src/os/win32 and src/os/posix hold one implementation each (include/nxrt/os/README.md); only the folder of this platform is
# built, picked up by a glob, so a new file there needs no CMake edit. The shared src/os/*.cpp (sync.cpp, ...) is in the glob above.
list(FILTER NXRT_SOURCES EXCLUDE REGEX "/src/os/(win32|posix)/")
nx_platform_os_sources(NXRT_OS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/os")
list(APPEND NXRT_SOURCES ${NXRT_OS_SOURCES})
add_library(nxrt STATIC ${NXRT_SOURCES})
include(cmake/sdl_dep.cmake)   # the audio sink (src/services/audio_sdl.cpp) plays through SDL on every platform
target_link_libraries(nxrt PUBLIC SDL3::SDL3-static)
# formats/nx_formats.h: the file formats shared with the setup tool (BC7 cache key and header, programs.bin)
target_include_directories(nxrt PUBLIC include "${NX_BASE_DIR}/formats")
if(EXISTS "${NXRT_RECOMP_INCLUDE}")
    target_include_directories(nxrt PUBLIC "${NXRT_RECOMP_INCLUDE}")
endif()
nx_platform_target_system_libs(nxrt)

if(NXRT_BUILD_TESTS)
    enable_testing()
    include(cmake/tests.cmake)
    # The folder of the game whose local test data (work/shader_cache, work/nvn_refs, game/romfs) the data-driven tests read
    # (tests/project_root.h); empty: those tests skip. The NXRT_TEST_DATA_GAME_DIR environment variable overrides it at run time.
    set(NXRT_TEST_DATA_GAME_DIR "" CACHE PATH "game folder with the optional test data (games/<id>)")
    # The neutral fixture game the base tests register (tests/fixture_game): small fake tables, no title.
    add_library(nxrt_fixture_game STATIC tests/fixture_game/fixture_game.cpp)
    target_include_directories(nxrt_fixture_game PUBLIC include tests/fixture_game)
    target_compile_features(nxrt_fixture_game PUBLIC cxx_std_20)
    file(GLOB NXRT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    list(FILTER NXRT_TEST_SOURCES EXCLUDE REGEX "/tests/test_main[.]cpp$")
    # Windows: a linker map next to the exe (the crash handler and the watchdog name host code from it), low host allocations
    # (HIGHENTROPYVA:NO) so the guest ranges at 0x71_0000_0000 and up are free, a 64 MiB main stack (nx_platform_exe_link_options).
    nx_platform_exe_link_options(_nxrt_tests_link STACK 67108864 LOW_VA)
    nxrt_add_test_exe(nxrt_tests SOURCES ${NXRT_TEST_SOURCES} LINK nxrt nxrt_fixture_game REGISTER nxrt_register_fixture_game
                      DEFINES NXRT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
                      LINK_OPTIONS ${_nxrt_tests_link})

    # Lifted-code integration tests (tests/lift): need the recompiler, its Python environment and the AArch64 clang.
    option(NXRT_BUILD_LIFT_TESTS "Lift test programs with nxrecomp and run them on the runtime (CTest entries lift_*)" ON)
    # tools/.venv is local only: a worktree uses the main checkout's
    if(NXRT_BUILD_LIFT_TESTS AND (EXISTS "${NX_BASE_DIR}/tools/.venv/${NX_VENV_PYTHON_REL}" OR EXISTS "${NX_ROOT}/base/tools/.venv/${NX_VENV_PYTHON_REL}"))
        add_subdirectory(tests/lift)
    else()
        message(STATUS "nxrt: lifted-code integration tests (tests/lift) are off: tools/.venv not found")
    endif()
endif()

if(NXRT_BUILD_GFX)
    include(cmake/gfx.cmake)
endif()

# The game modules: games/<id>/runtime/CMakeLists.txt, configured with the game's manifest read (NX_GAME_*) and its generated header
# (nx_game.h, NXGAME_* macros) in NX_GAME_GENERATED_DIR. NXRT_SOURCE_DIR is this folder.
set(NXRT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "the runtime source folder")
foreach(_gd ${NXRT_GAME_DIRS})
    nx_game_read("${_gd}" NX_GAME)
    set(NX_GAME_GENERATED_DIR "${CMAKE_BINARY_DIR}/games/${NX_GAME_ID}/generated")
    nx_game_header(NX_GAME "${NX_GAME_GENERATED_DIR}/nx_game.h")
    add_subdirectory("${NX_GAME_DIR}/runtime" "${CMAKE_BINARY_DIR}/games/${NX_GAME_ID}")
endforeach()
include(cmake/game_exe.cmake)
