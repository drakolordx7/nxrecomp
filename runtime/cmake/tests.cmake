# SPDX-License-Identifier: GPL-3.0-or-later
# nxrt_add_test_exe(<target> SOURCES <files...> [LINK <libs...>] [INCLUDES <dirs...>] [DEFINES <defs...>] [REGISTER <function>]
#                   [TIMEOUT <s>] [ENVIRONMENT <VAR=value...>] [LINK_OPTIONS <opts...>])
# One test executable on the base's test framework (tests/test_framework.h, tests/test_main.cpp) and one CTest entry per test group: the
# group names are read at configure time from the NXTEST_GROUP lines of the sources, so CMake needs no edit when a group is added.
# REGISTER names the function that registers the game the tests run with (nxrt/game.h); test_main.cpp calls it before the first test.
# The base's own tests register the neutral fixture game (tests/fixture_game); a game's tests (games/<id>/runtime/tests) register the game.
include_guard(GLOBAL)
set(NXRT_TESTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../tests")

# nxrt_internal_for_tests: the one way a game's tests reach the base's private headers (the NVN layer, the play engine, the test tools, the
# launcher's editors, the NVN test harness tests/gfx). Such tests are white-box tests of the base that live in a game folder because they
# need the game's own data (its translated shaders); they link this target and nothing else adds an include path into src/.
add_library(nxrt_internal_for_tests INTERFACE)
target_include_directories(nxrt_internal_for_tests INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/../src/gfx" "${CMAKE_CURRENT_LIST_DIR}/../src/gfx/play" "${CMAKE_CURRENT_LIST_DIR}/../src/gfx/nvn"
    "${CMAKE_CURRENT_LIST_DIR}/../src/testtools" "${CMAKE_CURRENT_LIST_DIR}/../apps/launcher" "${NXRT_TESTS_DIR}/gfx")

function(nxrt_add_test_exe target)
    cmake_parse_arguments(A "" "REGISTER;TIMEOUT" "SOURCES;LINK;INCLUDES;DEFINES;ENVIRONMENT;LINK_OPTIONS" ${ARGN})
    if(NOT A_TIMEOUT)
        set(A_TIMEOUT 120)
    endif()
    add_executable(${target} "${NXRT_TESTS_DIR}/test_main.cpp" ${A_SOURCES})
    target_include_directories(${target} PRIVATE "${NXRT_TESTS_DIR}" ${A_INCLUDES})
    target_link_libraries(${target} PRIVATE ${A_LINK})
    if(A_DEFINES)
        target_compile_definitions(${target} PRIVATE ${A_DEFINES})
    endif()
    if(A_REGISTER)
        target_compile_definitions(${target} PRIVATE NXRT_TEST_GAME_REGISTER=${A_REGISTER})
    endif()
    # Where a game's optional test data lives (tests/project_root.h): NX_ROOT, and the game folder whose work/ and game/ hold it.
    target_compile_definitions(${target} PRIVATE NXRT_NX_ROOT="${NX_ROOT}" NXRT_TEST_DATA_GAME_DIR="${NXRT_TEST_DATA_GAME_DIR}")
    if(A_LINK_OPTIONS)
        target_link_options(${target} PRIVATE ${A_LINK_OPTIONS})
    endif()
    set(groups "")
    foreach(src ${A_SOURCES})
        file(STRINGS "${src}" lines REGEX "^NXTEST_GROUP[(]")
        foreach(l ${lines})
            string(REGEX REPLACE "^NXTEST_GROUP[(]([a-z0-9_]+)[)].*$" "\\1" g "${l}")
            list(APPEND groups ${g})
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES groups)
    foreach(g ${groups})
        add_test(NAME ${g} COMMAND ${target} ${g} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(${g} PROPERTIES TIMEOUT ${A_TIMEOUT})
        if(A_ENVIRONMENT)
            set_tests_properties(${g} PROPERTIES ENVIRONMENT "${A_ENVIRONMENT}")
        endif()
    endforeach()
    message(STATUS "${target} test groups: ${groups}")
endfunction()
