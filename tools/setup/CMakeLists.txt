# The setup tool: first-run setup of one game (dump -> game folder), built for the game named by NX_GAME_DIR. Windows x64, clang-cl + Ninja.
# The base installer extracts the dump; what only one game needs (its texture list, program list, ini lines: src/game_setup.h) is that
# game's hook, a CMake file the manifest names ("setup_hook") and this file includes.
#   pwsh -File tools\setup\scripts\build.ps1 [-Clean]
cmake_minimum_required(VERSION 3.20)
# Static MSVC runtime (/MT): players need no Visual C++ Redistributable. Set before project() so every target below, third-party code
# included, uses it; the libraries this one links must agree (a mismatch is a link error).
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
project(nx_setup C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# The release version the setup log and window show ("dev" for local builds; scripts/build.ps1 -Version sets it).
set(NXS_VERSION "dev" CACHE STRING "release version shown by the setup programs")
add_compile_definitions(NXS_VERSION="${NXS_VERSION}")
enable_testing()

# Libraries from the S4 tools (built in-tree, not installed): ASTC->BC7 transcoder and Tegra tiling.
set(NXS_TEXTURES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../textures CACHE PATH "tools/textures")
set(NXS_TILING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../tiling CACHE PATH "tools/tiling")
add_subdirectory(${NXS_TEXTURES_DIR} ${CMAKE_BINARY_DIR}/textures EXCLUDE_FROM_ALL)
add_subdirectory(${NXS_TILING_DIR} ${CMAKE_BINARY_DIR}/tiling EXCLUDE_FROM_ALL)

set(S ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(NXS_WARN /W3 -Wno-unused-parameter)

function(nxs_lib name)
    set(srcs "")
    foreach(f ${ARGN})
        list(APPEND srcs ${S}/${f})
    endforeach()
    add_library(${name} STATIC ${srcs})
    target_include_directories(${name} PUBLIC ${S})
    target_compile_definitions(${name} PUBLIC _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    target_compile_options(${name} PRIVATE ${NXS_WARN})
endfunction()

# The game this setup is built for (src/game.h): its title id, builds and file names, from its manifest <NX_GAME_DIR>/game.json
# (base cmake/nx_game.cmake), generated into nx_game.h and read by src/game_profile.cpp. The setup programs are named after the
# manifest (files.setup, files.setup_cli: <id>_setup.exe, <id>_setup_cli.exe); their file properties show its product name.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nx_game.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../recomp/cmake/nx_toolchain.cmake)   # NX_PYTHON
set(NX_GAME_DIR "" CACHE PATH "the game folder (games/<id>) whose manifest game.json the setup tool is built for")
if(NOT NX_GAME_DIR)
    message(FATAL_ERROR "NX_GAME_DIR is not set: pass -DNX_GAME_DIR=<games/<id>> (the folder with game.json)")
endif()
nx_game_read("${NX_GAME_DIR}" NXS_G)
nx_game_header(NXS_G "${CMAKE_BINARY_DIR}/generated/nx_game.h")
get_filename_component(NXS_EXE "${NXS_G_SETUP}" NAME_WE)
get_filename_component(NXS_EXE_CLI "${NXS_G_SETUP_CLI}" NAME_WE)
set(NXS_PRODUCT "${NXS_G_PRODUCT}")
# The GUI's manifest (gui/setup.manifest.in), named after the game; gui/setup.rc finds it on the resource include path.
string(REGEX REPLACE "[^A-Za-z0-9]" "" NXS_ASSEMBLY "${NXS_G_TITLE}")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/gui/setup.manifest.in ${CMAKE_BINARY_DIR}/generated/setup.manifest @ONLY)
add_compile_definitions(NXS_EXE_NAME="${NXS_EXE}")
nxs_lib(nxs_base util.cpp storage.cpp log.cpp game_profile.cpp)
target_include_directories(nxs_base PRIVATE "${CMAKE_BINARY_DIR}/generated")
# formats/nx_formats.h: the file formats shared with the runtime (BC7 cache key, programs.bin, shader pack)
target_include_directories(nxs_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../formats)
nxs_lib(nxs_crypto crypto.cpp fileio.cpp)
target_link_libraries(nxs_crypto PUBLIC nxs_base bcrypt)
nxs_lib(nxs_fs pfs.cpp romfs.cpp)
target_link_libraries(nxs_fs PUBLIC nxs_base nxs_crypto)
# The rest of the core (keys, NCA, containers, the generic texture cache, shader cache, pipeline) lives in nxs_core.
nxs_lib(nxs_core keys.cpp bktr.cpp nca.cpp container.cpp nso.cpp texcache.cpp shadercache.cpp pipeline.cpp ncz.cpp)
target_link_libraries(nxs_core PUBLIC nxs_base nxs_crypto nxs_fs tegra_tiling astc2bc7 shlwapi ole32 shell32)
# miniz 3.1.2 (MIT, third_party/miniz, from the official release): zlib inflate, for the game hooks that read zlib streams (a hook links miniz_lib)
add_library(miniz_lib STATIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(miniz_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz)
target_compile_definitions(miniz_lib PRIVATE _CRT_SECURE_NO_WARNINGS)
add_dependencies(nxs_core astc2bc7_cli)   # builds the three astcenc decoder DLLs next to the executables
# BC5 encoder (rgbcx) and the reference BC7 decoder (the oracle's): two-channel normal maps and the RGBA8 fallback of texcache.cpp
set(BC7_TP ${NXS_TEXTURES_DIR}/third_party/bc7enc_rdo)
target_sources(nxs_core PRIVATE ${BC7_TP}/rgbcx.cpp ${BC7_TP}/bc7decomp_ref.cpp)
target_include_directories(nxs_core PRIVATE ${BC7_TP})

# zstd (BSD-3-Clause; tools/setup/scripts/fetch_zstd.ps1 puts it in third_party/zstd): NCZ/NSZ/XCZ support and shader packs
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib/zstd.h)
    file(GLOB ZSTD_SRC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib/common/*.c
         ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib/compress/*.c ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib/decompress/*.c)
    add_library(zstd_lib STATIC ${ZSTD_SRC})
    target_include_directories(zstd_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib)
    target_compile_definitions(zstd_lib PRIVATE ZSTD_DISABLE_ASM=1 _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(nxs_core PUBLIC zstd_lib)
    target_compile_definitions(nxs_core PUBLIC NXS_HAVE_ZSTD=1)
endif()

# ---- tests: tests/test_<name>.cpp -> test_<name>.exe; libraries listed per test below, everything else gets nxs_core
# nxs_test_in(<dir> <name> <libs...>): the test <dir>/test_<name>.cpp. A game's hook adds its own tests with its own tests folder.
function(nxs_test_in dir name)
    set(src ${dir}/test_${name}.cpp)
    file(GLOB helpers ${CMAKE_CURRENT_SOURCE_DIR}/tests/synth*.cpp)
    add_executable(test_${name} ${src} ${helpers})
    target_include_directories(test_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${S})
    list(GET NXS_G_BUILD_IDS 0 _nxs_first_build)
    target_compile_definitions(test_${name} PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NXS_TEST_GAME_DIR="${NX_GAME_DIR}"
                               NXS_TEST_PYTHON="${NX_PYTHON}" NXS_TEST_BUILD_ID="${_nxs_first_build}"
                               NXS_TEST_NX_ROOT="${NX_ROOT}" NXS_TEST_GAME_ID="${NXS_G_ID}")
    target_compile_options(test_${name} PRIVATE ${NXS_WARN})
    target_link_libraries(test_${name} PRIVATE ${ARGN})
    add_test(NAME ${name} COMMAND test_${name})
endfunction()
function(nxs_test name)
    nxs_test_in(${CMAKE_CURRENT_SOURCE_DIR}/tests ${name} ${ARGN})
endfunction()

# ---- the game's setup hook (src/game_setup.h). The manifest's "setup_hook" names a CMake file that defines the STATIC library
# nxs_game_hook (the game's GameSetup and what it reads: archives, formats), using nxs_base, nxs_crypto, miniz_lib, tegra_tiling, the variables
# NXS_SRC (this tool's src folder) and BC7_TP, nxs_lib, and nxs_test_in for its own tests. It must not link nxs_core (nxs_core links it).
# A game without a hook gets one that has no GameSetup: no texture cache, no program list.
if(NXS_G_SETUP_HOOK)
    set(NXS_SRC ${S})
    include("${NXS_G_SETUP_HOOK}")
    if(NOT TARGET nxs_game_hook)
        message(FATAL_ERROR "${NXS_G_SETUP_HOOK} does not define the target nxs_game_hook")
    endif()
else()
    nxs_lib(nxs_game_hook game_setup_none.cpp)
    target_link_libraries(nxs_game_hook PUBLIC nxs_base)
endif()
target_link_libraries(nxs_core PUBLIC nxs_game_hook)

nxs_test(crypto nxs_crypto nxs_base)
nxs_test(pfs nxs_fs nxs_crypto nxs_base)
nxs_test(romfs nxs_fs nxs_crypto nxs_base)
foreach(t keys nca container update formats shadercache pipeline nso zstd realdump)
    nxs_test(${t} nxs_core)
endforeach()

# ---- executables
add_executable(nx_setup_cli cli/main.cpp)
target_link_libraries(nx_setup_cli PRIVATE nxs_core)
target_compile_definitions(nx_setup_cli PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(nx_setup_cli PRIVATE ${NXS_WARN})
set_target_properties(nx_setup_cli PROPERTIES OUTPUT_NAME "${NXS_EXE_CLI}")
foreach(isa avx2 sse4.1 sse2)
    add_custom_command(TARGET nx_setup_cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:astcdec-${isa}-shared> $<TARGET_FILE_DIR:nx_setup_cli>)
endforeach()
option(NXS_GUI_SIM "Build the GUI against gui/sim_pipeline.cpp (fake progress) instead of the real core" OFF)
if(NXS_GUI_SIM)
    add_executable(nx_setup WIN32 gui/main_win32.cpp gui/sim_pipeline.cpp gui/setup.rc)
    target_link_libraries(nx_setup PRIVATE nxs_base)
else()
    add_executable(nx_setup WIN32 gui/main_win32.cpp gui/setup.rc)
    target_link_libraries(nx_setup PRIVATE nxs_core)
endif()
target_link_libraries(nx_setup PRIVATE comctl32 uxtheme dwmapi shlwapi ole32 shell32)
target_compile_definitions(nx_setup PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(nx_setup PRIVATE ${NXS_WARN})
target_include_directories(nx_setup PRIVATE ${S})
target_link_options(nx_setup PRIVATE /MANIFEST:NO)   # gui/setup.rc carries the manifest
set_target_properties(nx_setup PROPERTIES OUTPUT_NAME "${NXS_EXE}")
set_source_files_properties(gui/setup.rc PROPERTIES COMPILE_DEFINITIONS "NXS_RC_EXE=\"${NXS_EXE}\";NXS_RC_PRODUCT=\"${NXS_PRODUCT}\""
    INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/generated" OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/generated/setup.manifest")
if(NOT NXS_GUI_SIM)
    foreach(isa avx2 sse4.1 sse2)
        add_custom_command(TARGET nx_setup POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:astcdec-${isa}-shared> $<TARGET_FILE_DIR:nx_setup>)
    endforeach()
endif()
