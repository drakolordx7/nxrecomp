# SPDX-License-Identifier: GPL-3.0-or-later
# The pre-game launcher (apps/launcher): SDL3 + Dear ImGui (base/ui, nxui), the same program on Windows and Linux, static runtime, no installer.
# It edits controls.ini with the same settings code the game uses (the nxrt_settings library, gfx.cmake), starts the setup tool for the
# first-time setup and starts the game exe. Included from gfx.cmake (needs the SDL3 target). Its branding and factory settings are those of
# one game module: the data library (names, controls.ini defaults) and the launcher resources (cmake/game_exe.cmake builds <id>_launcher per
# game). Without a game the base builds nxrt_launcher, unbranded and without resources.
# With the option off this file defines nothing, and the games' `if(TARGET nxrt_launcher_core)` guards skip their launcher pages.
#
option(NXRT_BUILD_LAUNCHER "Build the launcher (apps/launcher, SDL3 + Dear ImGui) and its tests" ON)
if(NOT NXRT_BUILD_LAUNCHER)
    return()
endif()
set(NXRT_LAUNCHER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/apps/launcher" CACHE INTERNAL "the launcher sources (nxrt_add_launcher runs in the game project)")

# ---- the SDL3 + Dear ImGui launcher ---------------------------------------------------------------------------------------------------
include("${NX_BASE_DIR}/ui/nxui.cmake")
# The few host calls the launcher makes (processes, files, time) without the rest of the runtime.
add_library(nxrt_launcher_os STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/os/${NX_OS_DIR}/process.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/os/${NX_OS_DIR}/fs.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/os/${NX_OS_DIR}/time.cpp")
target_include_directories(nxrt_launcher_os PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(nxrt_launcher_os PUBLIC cxx_std_20)
nx_platform_target_system_libs(nxrt_launcher_os)

add_library(nxrt_launcher_core STATIC
    "${NXRT_LAUNCHER_DIR}/form.cpp" "${NXRT_LAUNCHER_DIR}/game_info.cpp" "${NXRT_LAUNCHER_DIR}/bindings_edit.cpp" "${NXRT_LAUNCHER_DIR}/pad_capture.cpp")
# The public SDK (include/nxrt/game) is all a game module sees; the base's own tests reach the private headers through their own INCLUDES.
target_include_directories(nxrt_launcher_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${NX_BASE_DIR}/formats" PRIVATE "${NXRT_LAUNCHER_DIR}")
target_compile_definitions(nxrt_launcher_core PUBLIC NXRT_EXE_SUFFIX="${NX_EXE_SUFFIX}")
target_link_libraries(nxrt_launcher_core PUBLIC nxrt_settings nxrt_launcher_os)
set(NXRT_LAUNCHER_APP_SOURCES
    "${NXRT_LAUNCHER_DIR}/main.cpp" "${NXRT_LAUNCHER_DIR}/page_view.cpp" "${NXRT_LAUNCHER_DIR}/page_display.cpp" "${NXRT_LAUNCHER_DIR}/page_controls.cpp"
    "${NXRT_LAUNCHER_DIR}/page_keys.cpp" "${NXRT_LAUNCHER_DIR}/page_pad.cpp" "${NXRT_LAUNCHER_DIR}/game_options_ui.cpp" CACHE INTERNAL "the launcher exe sources (nxrt_add_launcher)")
set(NXRT_LAUNCHER_APP_LIBS nxui CACHE INTERNAL "")

# nxrt_add_launcher(<target> [VERSION <v>] [GAME_LIB <lib> REGISTER <function>] [RC <file>] [OUTPUT_NAME <name>] [OUTPUT_DIR <dir>])
# The launcher exe. VERSION is the release version it shows ("dev" for local builds). GAME_LIB/REGISTER: the game module's data library and
# its register function, called at the top of main. RC: the resource script (version info, manifest; Windows).
function(nxrt_add_launcher target)
    cmake_parse_arguments(A "" "VERSION;GAME_LIB;REGISTER;RC;OUTPUT_NAME;OUTPUT_DIR" "" ${ARGN})
    if(NOT A_VERSION)
        set(A_VERSION "dev")
    endif()
    if(NX_PLATFORM STREQUAL "win32")
        add_executable(${target} WIN32 ${NXRT_LAUNCHER_APP_SOURCES} ${A_RC})
    else()
        add_executable(${target} ${NXRT_LAUNCHER_APP_SOURCES} ${A_RC})
    endif()
    target_include_directories(${target} PRIVATE "${NXRT_LAUNCHER_DIR}")
    target_link_libraries(${target} PRIVATE nxrt_launcher_core ${NXRT_LAUNCHER_APP_LIBS})
    target_compile_definitions(${target} PRIVATE NXRT_VERSION="${A_VERSION}" ${NX_RT_COMPILE_DEFINITIONS})   # (the project of a game exe is outside the runtime folder: the Windows header switches are not inherited)
    if(A_GAME_LIB)
        target_link_libraries(${target} PRIVATE ${A_GAME_LIB})
        target_compile_definitions(${target} PRIVATE NXRT_LAUNCHER_GAME_REGISTER=${A_REGISTER})
    endif()
    if(NX_PLATFORM STREQUAL "win32")
        target_link_options(${target} PRIVATE /ENTRY:mainCRTStartup)   # a GUI program with main(), not WinMain
    endif()
    if(A_RC AND NX_PLATFORM STREQUAL "win32")
        target_link_options(${target} PRIVATE /MANIFEST:NO)   # the resource script carries the manifest
    endif()
    if(A_OUTPUT_NAME)
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${A_OUTPUT_NAME}")
    endif()
    if(A_OUTPUT_DIR)
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${A_OUTPUT_DIR}")
    endif()
endfunction()

nxrt_add_launcher(nxrt_launcher)

if(NXRT_BUILD_TESTS)
    file(GLOB NXRT_LAUNCHER_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher/*.cpp")
    nxrt_add_test_exe(nxrt_launcher_tests SOURCES ${NXRT_LAUNCHER_TEST_SOURCES} LINK nxrt_launcher_core nxrt_fixture_game
                      REGISTER nxrt_register_fixture_game INCLUDES "${NXRT_LAUNCHER_DIR}")
endif()
