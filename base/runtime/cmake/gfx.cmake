# SPDX-License-Identifier: GPL-3.0-or-later
# Graphics shell (S4b): SDL3 platform + input queue, Vulkan 1.3 device layer, NVN front end, ImGui overlay.
# Built as the static library nxrt_gfx (links nxrt) and the test executable nxrt_gfx_tests (CTest groups gfx_*).
include(${CMAKE_CURRENT_LIST_DIR}/gfx_deps.cmake)

# The offline tiling library of base/tools is compiled in as source (tools/tiling, tegra_tiling.h).
set(NXRT_TOOLS_DIR "${NX_BASE_DIR}/tools" CACHE PATH "base/tools (tools/tiling is used)")
if(NOT EXISTS "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp")
    message(FATAL_ERROR "tools/tiling not found: set NXRT_TOOLS_DIR to the tools repository (looked in ${NXRT_TOOLS_DIR})")
endif()
include("${NXRT_TOOLS_DIR}/setup/storage/nx_setup_storage.cmake")   # nx_setup_storage: the setup tool's byte sources, for a game module that reads archives
add_library(nxrt_tiling STATIC "${NXRT_TOOLS_DIR}/tiling/src/tegra_tiling.cpp")
target_include_directories(nxrt_tiling PUBLIC "${NXRT_TOOLS_DIR}/tiling/include")

# ASTC -> BC7 transcoder (tools/textures): the prebuilt static libraries (tools/textures/scripts/build.ps1, output NX/build/textures; Linux:
# build/textures/linux) and the decoder DLLs/shared objects that astc2bc7 loads at run time (copied next to every executable that links nxrt_gfx).
# Linux without a prebuilt copy builds tools/textures in the tree when ispc is available (PATH or -DNXRT_ISPC=); otherwise the library is left
# out (a warning) and only the link of an exe that uses it fails.
if(NX_PLATFORM STREQUAL "win32")
    set(_nxrt_a2b_sub "")
else()
    set(_nxrt_a2b_sub "/linux")
endif()
nx_platform_static_lib_path(_nxrt_a2b_lib "" astc2bc7)
nx_platform_static_lib_path(_nxrt_bc7e_lib "" bc7e_ispc)
if(NX_ROOT AND EXISTS "${NX_ROOT}/build/textures${_nxrt_a2b_sub}${_nxrt_a2b_lib}")
    set(_nxrt_a2b "${NX_ROOT}/build/textures${_nxrt_a2b_sub}")
else()
    set(_nxrt_a2b "${NXRT_TOOLS_DIR}/textures/build/release")
endif()
set(NXRT_A2B_DIR "${_nxrt_a2b}" CACHE PATH "Build folder of tools/textures (astc2bc7, bc7e_ispc, astcdec-*-shared)")
add_library(nxrt_astc2bc7 INTERFACE)
target_include_directories(nxrt_astc2bc7 INTERFACE "${NXRT_TOOLS_DIR}/textures/include")
if(EXISTS "${NXRT_A2B_DIR}${_nxrt_a2b_lib}" AND EXISTS "${NXRT_A2B_DIR}${_nxrt_bc7e_lib}")
    target_link_libraries(nxrt_astc2bc7 INTERFACE "${NXRT_A2B_DIR}${_nxrt_a2b_lib}" "${NXRT_A2B_DIR}${_nxrt_bc7e_lib}")
    file(GLOB _nxrt_a2b_dlls "${NXRT_A2B_DIR}/${NX_SHARED_LIB_PREFIX}astcdec-*-shared${NX_SHARED_LIB_SUFFIX}")
    set(NXRT_A2B_DLLS "${_nxrt_a2b_dlls}" CACHE INTERNAL "the ASTC decoder libraries, copied next to every exe that links nxrt_gfx")
elseif(NX_PLATFORM STREQUAL "win32")
    message(FATAL_ERROR "astc2bc7.lib not found in ${NXRT_A2B_DIR}: build tools/textures first (pwsh tools/textures/scripts/build.ps1) or set NXRT_A2B_DIR")
else()
    find_program(NXRT_ISPC NAMES ispc DOC "Intel ISPC compiler (builds tools/textures in the tree)")
    if(NXRT_ISPC)
        set(A2B_ISPC "${NXRT_ISPC}" CACHE FILEPATH "" FORCE)
        add_subdirectory("${NXRT_TOOLS_DIR}/textures" "${CMAKE_BINARY_DIR}/textures" EXCLUDE_FROM_ALL)
        target_link_libraries(nxrt_astc2bc7 INTERFACE astc2bc7)
        set(NXRT_A2B_DLLS "$<TARGET_FILE:astcdec-avx2-shared>;$<TARGET_FILE:astcdec-sse4.1-shared>;$<TARGET_FILE:astcdec-sse2-shared>"
            CACHE INTERNAL "the ASTC decoder libraries, copied next to every exe that links nxrt_gfx")
        set(NXRT_A2B_TARGETS astcdec-avx2-shared astcdec-sse4.1-shared astcdec-sse2-shared)
    else()
        message(WARNING "astc2bc7 not found in ${NXRT_A2B_DIR} and no ispc to build it: executables that use the ASTC transcoder will not link")
        set(NXRT_A2B_DLLS "" CACHE INTERNAL "")
    endif()
endif()

# The play-mode settings model (types, ini parse and format, key names): the game's play mode and the launcher edit the same file with it.
add_library(nxrt_settings STATIC src/gfx/play/play_types.cpp src/gfx/play/play_config.cpp)
target_include_directories(nxrt_settings PUBLIC include)
target_link_libraries(nxrt_settings PUBLIC SDL3::SDL3-static)

file(GLOB_RECURSE NXRT_GFX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/*.cpp")
list(FILTER NXRT_GFX_SOURCES EXCLUDE REGEX "/play/play_(types|config)[.]cpp$")   # nxrt_settings
add_library(nxrt_gfx STATIC ${NXRT_GFX_SOURCES})
if(NXRT_A2B_TARGETS)
    add_dependencies(nxrt_gfx ${NXRT_A2B_TARGETS})   # the decoder libraries the exes copy
endif()
target_include_directories(nxrt_gfx PUBLIC include src/gfx/nvn/third_party PRIVATE src/gfx src/gfx/nvn)
target_link_libraries(nxrt_gfx PUBLIC nxrt nxrt_settings nxrt_volk nxrt_vma nxrt_imgui nxrt_tiling nxrt_astc2bc7 SDL3::SDL3-static)
target_compile_definitions(nxrt_gfx PUBLIC VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

if(NXRT_BUILD_TESTS)
    file(GLOB_RECURSE NXRT_GFX_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gfx/*.cpp")
    set(_gfx_defs NXRT_GFX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/gfx")
    set(_gfx_srcs ${NXRT_GFX_TEST_SOURCES})
    # The BC7 reference decoder of bc7enc_rdo (public domain; the same file the setup tool checks its texture cache with): the ASTC texture scene
    # works out its expected picture from the current encoder instead of a stored image (tests/gfx/test_nvn_scene_texture.cpp).
    set(NXRT_BC7_REF_DIR "${NXRT_TOOLS_DIR}/textures/third_party/bc7enc_rdo")
if(NOT EXISTS "${NXRT_BC7_REF_DIR}" AND NX_ROOT)   # third_party/ is local only: a worktree reads the main checkout's
        set(NXRT_BC7_REF_DIR "${NX_ROOT}/base/tools/textures/third_party/bc7enc_rdo")
endif()
    set(_gfx_incs src/gfx src/gfx/nvn)
    if(EXISTS "${NXRT_BC7_REF_DIR}/bc7decomp_ref.cpp")
        list(APPEND _gfx_srcs "${NXRT_BC7_REF_DIR}/bc7decomp_ref.cpp")
        list(APPEND _gfx_incs "${NXRT_BC7_REF_DIR}")
        list(APPEND _gfx_defs NXRT_HAVE_BC7_REF=1)
    endif()
    nx_platform_exe_link_options(_gfx_tests_link STACK 8388608)
    nxrt_add_test_exe(nxrt_gfx_tests SOURCES ${_gfx_srcs} LINK nxrt_gfx nxrt_fixture_game REGISTER nxrt_register_fixture_game
                      INCLUDES ${_gfx_incs} DEFINES ${_gfx_defs} TIMEOUT 300 ENVIRONMENT "NXRT_GFX_HIDDEN=1"
                      LINK_OPTIONS ${_gfx_tests_link})
    foreach(dll ${NXRT_A2B_DLLS})
        add_custom_command(TARGET nxrt_gfx_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:nxrt_gfx_tests>")
    endforeach()
    # Timing oracles must not share the machine with another test of this suite.
    foreach(g gfx_limiter gfx_pacing)
        if(TEST ${g})
            set_tests_properties(${g} PROPERTIES RUN_SERIAL TRUE)
        endif()
    endforeach()
    if(TEST gfx_pacing)
        set_tests_properties(gfx_pacing PROPERTIES TIMEOUT 900)   # up to four attempts per case on a busy GPU
    endif()
endif()

include(${CMAKE_CURRENT_LIST_DIR}/launcher.cmake)
