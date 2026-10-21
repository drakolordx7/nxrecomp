# SPDX-License-Identifier: GPL-3.0-or-later
# Graphics shell (S4b): SDL3 platform + input queue, Vulkan 1.3 device layer, NVN front end, ImGui overlay.
# Built as the static library nxrt_gfx (links nxrt) and the test executable nxrt_gfx_tests (CTest groups gfx_*).
include(${CMAKE_CURRENT_LIST_DIR}/gfx_deps.cmake)

# The offline tiling library of the tools repository is compiled in as source (tools/tiling, tegra_tiling.h): found at
# ../tools or, from a worktree under runtime-wt/<name>, at ../../tools.
set(NXRT_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools" CACHE PATH "The tools repository (tools/tiling is used)")
if(NOT EXISTS "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/tiling/src/tegra_tiling.cpp")
    get_filename_component(_nxrt_td "${CMAKE_CURRENT_SOURCE_DIR}/../../tools" ABSOLUTE)
    set(NXRT_TOOLS_DIR "${_nxrt_td}" CACHE PATH "The tools repository (tools/tiling is used)" FORCE)
endif()
if(NOT EXISTS "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp")
    message(FATAL_ERROR "tools/tiling not found: set NXRT_TOOLS_DIR to the tools repository (looked in ${NXRT_TOOLS_DIR})")
endif()
add_library(nxrt_tiling STATIC "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp")
target_include_directories(nxrt_tiling PUBLIC "${NXRT_TOOLS_DIR}/tiling/include")

file(GLOB_RECURSE NXRT_GFX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/*.cpp")
add_library(nxrt_gfx STATIC ${NXRT_GFX_SOURCES})
target_include_directories(nxrt_gfx PUBLIC include PRIVATE src/gfx)
target_link_libraries(nxrt_gfx PUBLIC nxrt nxrt_volk nxrt_vma nxrt_imgui nxrt_tiling SDL3::SDL3-static)
target_compile_definitions(nxrt_gfx PUBLIC VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=0)

if(NXRT_BUILD_TESTS)
    file(GLOB_RECURSE NXRT_GFX_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gfx/*.cpp")
    add_executable(nxrt_gfx_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main.cpp" ${NXRT_GFX_TEST_SOURCES})
    target_include_directories(nxrt_gfx_tests PRIVATE tests src/gfx)
    target_link_libraries(nxrt_gfx_tests PRIVATE nxrt_gfx)
    target_compile_definitions(nxrt_gfx_tests PRIVATE NXRT_GFX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/gfx")
    target_link_options(nxrt_gfx_tests PRIVATE /MAP /STACK:8388608 /DEBUG)

    set(NXRT_GFX_GROUPS "")
    foreach(src ${NXRT_GFX_TEST_SOURCES})
        file(STRINGS "${src}" lines REGEX "^NXTEST_GROUP[(]")
        foreach(l ${lines})
            string(REGEX REPLACE "^NXTEST_GROUP[(]([a-z0-9_]+)[)].*$" "\\1" g "${l}")
            list(APPEND NXRT_GFX_GROUPS ${g})
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES NXRT_GFX_GROUPS)
    foreach(g ${NXRT_GFX_GROUPS})
        add_test(NAME ${g} COMMAND nxrt_gfx_tests ${g} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(${g} PROPERTIES TIMEOUT 180 ENVIRONMENT "NXRT_GFX_HIDDEN=1")
    endforeach()
    message(STATUS "nxrt gfx test groups: ${NXRT_GFX_GROUPS}")
endif()
