# SPDX-License-Identifier: GPL-3.0-or-later
# nxrt: game-independent runtime skeleton (OS/HLE layer) for the Metroid Prime Remastered static recompilation.
# Build: scripts\build.ps1   (clang-cl + Ninja, Release)   Test: ctest --test-dir build\release
cmake_minimum_required(VERSION 3.25)
project(nxrt C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(NOT WIN32)
    message(FATAL_ERROR "nxrt targets native Windows x64 only")
endif()

option(NXRT_BUILD_TESTS "Build the nxrt_tests executable and register its CTest entries" ON)

# The generated-code ABI header (Ctx, call_indirect, ...) is owned by stream S1 (recomp/include/nxrecomp/).
set(NXRT_RECOMP_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/include" CACHE PATH "Directory that holds nxrecomp/")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(/W3 -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type-mismatch
                        -Wno-cast-function-type-strict -Wno-language-extension-token)
endif()
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)

file(GLOB_RECURSE NXRT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(nxrt STATIC ${NXRT_SOURCES})
target_include_directories(nxrt PUBLIC include)
if(EXISTS "${NXRT_RECOMP_INCLUDE}")
    target_include_directories(nxrt PUBLIC "${NXRT_RECOMP_INCLUDE}")
endif()
target_link_libraries(nxrt PUBLIC psapi)

if(NXRT_BUILD_TESTS)
    enable_testing()
    file(GLOB_RECURSE NXRT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    add_executable(nxrt_tests ${NXRT_TEST_SOURCES})
    target_link_libraries(nxrt_tests PRIVATE nxrt)
    target_compile_definitions(nxrt_tests PRIVATE NXRT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    # A linker map next to the exe: the crash handler and the watchdog name host code from it. Keep host allocations low
    # (HIGHENTROPYVA:NO) so the guest ranges at 0x71_0000_0000 and up are free; a 64 MiB main stack.
    target_link_options(nxrt_tests PRIVATE /MAP /STACK:67108864 /HIGHENTROPYVA:NO /DEBUG)

    # One CTest entry per test group; the group names are listed by `nxrt_tests --list`. The list is read at configure
    # time from the NXTEST_GROUP lines of the test sources so that CMake needs no edit when a group is added.
    set(NXRT_GROUPS "")
    foreach(src ${NXRT_TEST_SOURCES})
        file(STRINGS "${src}" lines REGEX "^NXTEST_GROUP\\(")
        foreach(l ${lines})
            string(REGEX REPLACE "^NXTEST_GROUP\\(([a-z0-9_]+)\\).*$" "\\1" g "${l}")
            list(APPEND NXRT_GROUPS ${g})
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES NXRT_GROUPS)
    foreach(g ${NXRT_GROUPS})
        add_test(NAME ${g} COMMAND nxrt_tests ${g} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(${g} PROPERTIES TIMEOUT 120)
    endforeach()
    message(STATUS "nxrt test groups: ${NXRT_GROUPS}")
endif()
