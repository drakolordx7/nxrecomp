# SPDX-License-Identifier: GPL-3.0-or-later
# The Age of Calamity game module (base runtime docs/GAME_BOUNDARY.md, hwaoc_game.h). Configured by the base runtime for each folder in
# NXRT_GAME_DIRS, with the manifest read (nx_game.h in NX_GAME_GENERATED_DIR).
set(_hw "${CMAKE_CURRENT_SOURCE_DIR}")
set(_rt "${NXRT_SOURCE_DIR}")
set(NXRT_GAME_HWAOC_INCLUDES "${_rt}/include" "${_hw}" "${NX_GAME_GENERATED_DIR}")

add_library(nxrt_game_hwaoc_data STATIC "${_hw}/hwaoc_data.cpp" "${_hw}/hwaoc_settings.cpp")
target_include_directories(nxrt_game_hwaoc_data PUBLIC ${NXRT_GAME_HWAOC_INCLUDES})
target_compile_features(nxrt_game_hwaoc_data PUBLIC cxx_std_20)

add_library(nxrt_game_hwaoc_core STATIC "${_hw}/hwaoc_core.cpp")
target_link_libraries(nxrt_game_hwaoc_core PUBLIC nxrt_game_hwaoc_data nxrt)

# the launcher's Resolution tab (neutral page API, nxrt/game/launcher_ui.h); only the launcher exe links it
if(TARGET nxrt_launcher_core AND NOT NXRT_LAUNCHER_LEGACY)
    add_library(nxrt_game_hwaoc_launcher STATIC "${_hw}/hwaoc_launcher.cpp")
    target_link_libraries(nxrt_game_hwaoc_launcher PUBLIC nxrt_game_hwaoc_data)
endif()

if(TARGET nxrt_gfx)
    add_library(nxrt_game_hwaoc STATIC "${_hw}/hwaoc_game.cpp" "${_hw}/hwaoc_play.cpp" "${_hw}/hwaoc_display.cpp")
    target_link_libraries(nxrt_game_hwaoc PUBLIC nxrt_game_hwaoc_core nxrt_gfx)
endif()
