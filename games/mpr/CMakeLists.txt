# SPDX-License-Identifier: GPL-3.0-or-later
# The Metroid Prime Remastered game module (docs/GAME_BOUNDARY.md, mpr_game.h): everything in the runtime that is about this one title. Three
# libraries, each a superset of the one before:
#   nxrt_game_mpr_data   identity, branding, factory controls.ini (pure data; the launcher links it)
#   nxrt_game_mpr_core   + title facts, frame-step sites, test-tooling probes (links nxrt)
#   nxrt_game_mpr        + play-mode hooks and button prompts (links nxrt_gfx; only with NXRT_BUILD_GFX). mpr.exe links this one.
# The headers of the base's play mode and test tooling are on the include path of all three (the hooks are written against them).
set(_mpr "${CMAKE_CURRENT_SOURCE_DIR}")
set(_rt "${PROJECT_SOURCE_DIR}")
set(NXRT_GAME_MPR_INCLUDES "${_rt}/include" "${_rt}/games" "${_mpr}" "${_rt}/src/gfx" "${_rt}/src/gfx/play" "${_rt}/src/testtools")

add_library(nxrt_game_mpr_data STATIC "${_mpr}/mpr_data.cpp" "${_mpr}/mpr_defaults.cpp" "${_mpr}/mpr_vocabulary.cpp")
target_include_directories(nxrt_game_mpr_data PUBLIC ${NXRT_GAME_MPR_INCLUDES})
target_compile_features(nxrt_game_mpr_data PUBLIC cxx_std_20)

add_library(nxrt_game_mpr_core STATIC "${_mpr}/mpr_core.cpp" "${_mpr}/mpr_tt_probe.cpp")
target_link_libraries(nxrt_game_mpr_core PUBLIC nxrt_game_mpr_data nxrt)

if(TARGET nxrt_gfx)
    # The setup tool's Retro pak and TXTR readers (tools/setup/src, MIT-derived format knowledge, same repository): the button prompts read
    # the game's icon textures from the player's own paks to recognise them (mpr_prompts.cpp).
    set(NXRT_SETUP_SRC "${NXRT_TOOLS_DIR}/setup/src")
    if(NOT EXISTS "${NXRT_SETUP_SRC}/pak.cpp")
        message(FATAL_ERROR "tools/setup/src/pak.cpp not found in ${NXRT_TOOLS_DIR}")
    endif()
    add_library(nxrt_mprpak STATIC "${NXRT_SETUP_SRC}/pak.cpp" "${NXRT_SETUP_SRC}/txtr.cpp" "${NXRT_SETUP_SRC}/storage.cpp" "${NXRT_SETUP_SRC}/util.cpp")
    target_include_directories(nxrt_mprpak PUBLIC "${NXRT_SETUP_SRC}")
    target_compile_features(nxrt_mprpak PUBLIC cxx_std_20)

    add_library(nxrt_game_mpr STATIC
        "${_mpr}/mpr_game.cpp" "${_mpr}/mpr_play.cpp" "${_mpr}/mpr_ui.cpp" "${_mpr}/mpr_prompts.cpp" "${_mpr}/mpr_tonemap.cpp"
        "${_mpr}/display_hooks.cpp" "${_mpr}/menu_mouse_game.cpp" "${_mpr}/play_aim.cpp" "${_mpr}/play_ballcam.cpp" "${_mpr}/play_lockcam.cpp" "${_mpr}/play_lookrate.cpp"
        "${_mpr}/play_scenefix.cpp" "${_mpr}/play_jump.cpp" "${_mpr}/play_glow.cpp" "${_mpr}/play_glow_ui.cpp" "${_mpr}/mpr_fastboot.cpp"
        "${_mpr}/mpr_tint.cpp" "${_mpr}/mpr_texedit.cpp")
    target_link_libraries(nxrt_game_mpr PUBLIC nxrt_game_mpr_core nxrt_gfx nxrt_mprpak)
endif()
