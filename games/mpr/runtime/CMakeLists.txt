# SPDX-License-Identifier: GPL-3.0-or-later
# The Metroid Prime Remastered game module (base runtime docs/GAME_BOUNDARY.md, mpr_game.h): everything in the runtime that is about this one
# title. Configured by the base runtime for each folder in NXRT_GAME_DIRS, with the manifest read (NX_GAME_*, nx_game.h in
# NX_GAME_GENERATED_DIR). Three libraries, each a superset of the one before:
#   nxrt_game_mpr_data   identity, branding, factory controls.ini (pure data; the launcher links it)
#   nxrt_game_mpr_core   + title facts, frame-step sites, test-tooling probes (links nxrt)
#   nxrt_game_mpr        + play-mode hooks, button prompts, boot hook (links nxrt_gfx; only with NXRT_BUILD_GFX). mpr.exe links this one.
# The headers of the base's play mode and test tooling are on the include path of all three (the hooks are written against them).
set(_mpr "${CMAKE_CURRENT_SOURCE_DIR}")
set(_rt "${NXRT_SOURCE_DIR}")
set(NXRT_GAME_MPR_INCLUDES "${_rt}/include" "${_mpr}" "${NX_GAME_GENERATED_DIR}" "${_rt}/src/gfx" "${_rt}/src/gfx/play" "${_rt}/src/testtools")

add_library(nxrt_game_mpr_data STATIC "${_mpr}/mpr_data.cpp" "${_mpr}/mpr_defaults.cpp" "${_mpr}/mpr_vocabulary.cpp")
target_include_directories(nxrt_game_mpr_data PUBLIC ${NXRT_GAME_MPR_INCLUDES})
target_compile_features(nxrt_game_mpr_data PUBLIC cxx_std_20)

add_library(nxrt_game_mpr_core STATIC "${_mpr}/mpr_core.cpp" "${_mpr}/mpr_tt_probe.cpp" "${_mpr}/mpr_tt_steps.cpp")
target_link_libraries(nxrt_game_mpr_core PUBLIC nxrt_game_mpr_data nxrt)

if(TARGET nxrt_gfx)
    # The setup tool's Retro pak and TXTR readers (base tools/setup/src, MIT-derived format knowledge): the button prompts read the game's icon
    # textures from the player's own paks to recognise them (mpr_prompts.cpp).
    set(NXRT_SETUP_SRC "${NXRT_TOOLS_DIR}/setup/src")
    if(NOT EXISTS "${NXRT_SETUP_SRC}/pak.cpp")
        message(FATAL_ERROR "tools/setup/src/pak.cpp not found in ${NXRT_TOOLS_DIR}")
    endif()
    add_library(nxrt_mprpak STATIC "${NXRT_SETUP_SRC}/pak.cpp" "${NXRT_SETUP_SRC}/txtr.cpp" "${NXRT_SETUP_SRC}/storage.cpp" "${NXRT_SETUP_SRC}/util.cpp")
    target_include_directories(nxrt_mprpak PUBLIC "${NXRT_SETUP_SRC}")
    target_compile_features(nxrt_mprpak PUBLIC cxx_std_20)

    add_library(nxrt_game_mpr STATIC
        "${_mpr}/mpr_game.cpp" "${_mpr}/mpr_boot.cpp" "${_mpr}/mpr_play.cpp" "${_mpr}/mpr_ui.cpp" "${_mpr}/mpr_prompts.cpp" "${_mpr}/mpr_tonemap.cpp" "${_mpr}/mpr_render.cpp" "${_mpr}/mpr_tonemap_patch.cpp"
        "${_mpr}/display_hooks.cpp" "${_mpr}/menu_mouse_game.cpp" "${_mpr}/play_aim.cpp" "${_mpr}/play_ballcam.cpp" "${_mpr}/play_lockcam.cpp" "${_mpr}/play_lookrate.cpp"
        "${_mpr}/play_scenefix.cpp" "${_mpr}/play_jump.cpp" "${_mpr}/play_glow.cpp" "${_mpr}/play_glow_ui.cpp" "${_mpr}/mpr_fastboot.cpp"
        "${_mpr}/mpr_tint.cpp")
    target_link_libraries(nxrt_game_mpr PUBLIC nxrt_game_mpr_core nxrt_gfx nxrt_mprpak)
endif()

# The game's own tests, on the base's test framework (base runtime cmake/tests.cmake), each executable with the most of the module it links
# registered: the core part (tests/core: runtime-side code) and the whole module (tests/: play-mode hooks and the base tests that check
# Metroid Prime Remastered's tables).
if(NXRT_BUILD_TESTS)
    file(GLOB _mpr_core_tests CONFIGURE_DEPENDS "${_mpr}/tests/core/*.cpp")
    nxrt_add_test_exe(mpr_core_tests SOURCES ${_mpr_core_tests} LINK nxrt_game_mpr_core REGISTER nxrt_register_game_mpr_core
                      INCLUDES "${_mpr}/tests" DEFINES NXRT_TEST_DATA_DIR="${_rt}/tests/data"
                      LINK_OPTIONS /MAP /STACK:67108864 /HIGHENTROPYVA:NO /DEBUG)
    if(TARGET nxrt_game_mpr)
        # The tests that need this game's translated shaders (games/mpr/work/shader_cache; they skip without it) and name its shader hashes: the NVN draw
        # path scenes, the pipeline caches, the SPIR-V patches, the tone-map patch. The harness (tests/gfx/nvn_app.h) is the base's.
        file(GLOB _mpr_gfx_tests CONFIGURE_DEPENDS "${_mpr}/tests/*.cpp")
        set(_mpr_gfx_incs "${_mpr}/tests" "${_rt}/tests/gfx" "${_rt}/src/gfx" "${_rt}/src/gfx/nvn")
        set(_mpr_gfx_defs NXRT_GFX_TEST_DATA_DIR="${_rt}/tests/data/gfx")
        if(DEFINED NXRT_BC7_REF_DIR AND EXISTS "${NXRT_BC7_REF_DIR}/bc7decomp_ref.cpp")   # the ASTC scene's reference decoder (base cmake/gfx.cmake)
            list(APPEND _mpr_gfx_tests "${NXRT_BC7_REF_DIR}/bc7decomp_ref.cpp")
            list(APPEND _mpr_gfx_incs "${NXRT_BC7_REF_DIR}")
            list(APPEND _mpr_gfx_defs NXRT_HAVE_BC7_REF=1)
        endif()
        get_filename_component(NXRT_TEST_DATA_GAME_DIR "${_mpr}/.." ABSOLUTE)   # (this folder only) work/shader_cache, game/romfs of this game
        nxrt_add_test_exe(mpr_gfx_tests SOURCES ${_mpr_gfx_tests} LINK nxrt_game_mpr REGISTER nxrt_register_game_mpr
                          INCLUDES ${_mpr_gfx_incs} DEFINES ${_mpr_gfx_defs} TIMEOUT 300 ENVIRONMENT "NXRT_GFX_HIDDEN=1"
                          LINK_OPTIONS /MAP /STACK:8388608 /DEBUG)
        foreach(dll ${NXRT_A2B_DLLS})
            add_custom_command(TARGET mpr_gfx_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:mpr_gfx_tests>")
        endforeach()
    endif()
    file(GLOB _mpr_launcher_tests CONFIGURE_DEPENDS "${_mpr}/tests/launcher/*.cpp")
    if(_mpr_launcher_tests AND TARGET nxrt_launcher_core)
        nxrt_add_test_exe(mpr_launcher_tests SOURCES ${_mpr_launcher_tests} LINK nxrt_launcher_core nxrt_game_mpr_data
                          REGISTER nxrt_register_game_mpr_data INCLUDES "${_mpr}/tests")
    endif()
endif()
