# SPDX-License-Identifier: GPL-3.0-or-later
# nxrt_add_game_exe(<id> GAME_DIR <games/<id>> [LIB_DIR <dir>] [VERSION <v>])
# One game's programs, from its manifest (games/<id>/game.json, cmake/nx_game.cmake) and the base's templates:
#   <id>            the game exe (apps/game/game_main.cpp) linked with the lifted libraries (LIB_DIR/<id>_<module>.lib, as the recomp
#                   build writes them; default <game dir>/work/build/<id>) and the game module nxrt_game_<id> (NXRT_GAME_DIRS must list
#                   the game folder), with the resources from apps/game/game.rc.in + game.manifest.in
#   <id>_launcher   the launcher (cmake/launcher.cmake) branded by nxrt_game_<id>_data (or nxrt_game_<id>_launcher, when the game has
#                   launcher pages of its own), with the same resource templates
# VERSION is the release version ("0.1.0"; default "dev") shown by both programs and in their file properties. NXRT_RELEASE (runtime
# CMakeLists.txt) builds the package flavour: a Windows GUI program that looks for ./exefs, with no build paths in it.
# Both land in the build folder's top level, next to the ASTC decoder DLLs and fonts\ (the shared-font fallback).
include_guard(GLOBAL)
get_filename_component(_nxrt_gad "${CMAKE_CURRENT_LIST_DIR}/../apps/game" ABSOLUTE)
set(NXRT_GAME_APP_DIR "${_nxrt_gad}" CACHE INTERNAL "templates and main of the game exe")

function(nxrt_add_game_exe id)
    cmake_parse_arguments(A "" "GAME_DIR;LIB_DIR;VERSION" "" ${ARGN})
    nx_game_read("${A_GAME_DIR}" G)
    if(NOT G_ID STREQUAL id)
        message(FATAL_ERROR "nxrt_add_game_exe(${id}): ${G_DIR}/game.json is the manifest of '${G_ID}'")
    endif()
    if(NOT TARGET nxrt_game_${id})
        message(FATAL_ERROR "nxrt_add_game_exe(${id}): no game module nxrt_game_${id}: list ${G_DIR} in NXRT_GAME_DIRS")
    endif()
    if(NOT A_LIB_DIR)
        set(A_LIB_DIR "${G_DIR}/work/build/${id}")
    endif()
    if(NOT A_VERSION)
        set(A_VERSION "dev")
    endif()
    set(gen "${CMAKE_BINARY_DIR}/games/${id}/exe")
    set(out "${CMAKE_BINARY_DIR}")

    set(mods "${G_ENTRY_MODULE}")
    foreach(m ${G_MODULES})
        if(NOT m STREQUAL G_ENTRY_MODULE)
            list(APPEND mods ${m})
        endif()
    endforeach()
    set(inc "")
    set(libs "")
    foreach(m ${mods})
        if(NOT TARGET ${id}_${m})
            add_library(${id}_${m} STATIC IMPORTED GLOBAL)
            set_target_properties(${id}_${m} PROPERTIES IMPORTED_LOCATION "${A_LIB_DIR}/${id}_${m}.lib")
        endif()
        string(APPEND inc "NXGAME_MODULE(${m})\n")
        list(APPEND libs ${id}_${m})
    endforeach()
    file(CONFIGURE OUTPUT "${gen}/nxgame_modules.inc" CONTENT "${inc}")

    set(NXG_VERSION "${A_VERSION}")
    set(NXG_VER_MAJOR 0)
    set(NXG_VER_MINOR 0)
    set(NXG_VER_PATCH 0)
    if(A_VERSION MATCHES "^([0-9]+)[.]([0-9]+)[.]([0-9]+)")
        set(NXG_VER_MAJOR ${CMAKE_MATCH_1})
        set(NXG_VER_MINOR ${CMAKE_MATCH_2})
        set(NXG_VER_PATCH ${CMAKE_MATCH_3})
    endif()
    set(NXG_PRODUCT "${G_PRODUCT}")
    string(REGEX REPLACE "[^A-Za-z0-9]" "" NXG_ASSEMBLY "${G_TITLE}")

    get_filename_component(exe_stem "${G_EXE}" NAME_WE)
    get_filename_component(launcher_stem "${G_LAUNCHER}" NAME_WE)
    configure_file("${NXRT_GAME_APP_DIR}/game.manifest.in" "${gen}/${exe_stem}.manifest" @ONLY)
    configure_file("${NXRT_GAME_APP_DIR}/launcher.manifest.in" "${gen}/${launcher_stem}.manifest" @ONLY)
    set(NXG_EXE "${G_EXE}")
    set(NXG_INTERNAL "${exe_stem}")
    set(NXG_DESCRIPTION "${G_PRODUCT}")
    set(NXG_MANIFEST "${exe_stem}.manifest")
    configure_file("${NXRT_GAME_APP_DIR}/game.rc.in" "${gen}/${exe_stem}.rc" @ONLY)
    set(NXG_EXE "${G_LAUNCHER}")
    set(NXG_INTERNAL "${launcher_stem}")
    set(NXG_DESCRIPTION "${G_PRODUCT} - Launcher")
    set(NXG_MANIFEST "${launcher_stem}.manifest")
    configure_file("${NXRT_GAME_APP_DIR}/game.rc.in" "${gen}/${launcher_stem}.rc" @ONLY)

    if(NXRT_RELEASE)
        set(exefs_default "exefs")
        set(romfs_default "romfs")
    else()
        set(exefs_default "${G_DIR}/game/exefs")
        set(romfs_default "${G_DIR}/game/romfs")
    endif()
    add_executable(${id} "${NXRT_GAME_APP_DIR}/game_main.cpp" "${gen}/${exe_stem}.rc")
    set_source_files_properties("${gen}/${exe_stem}.rc" PROPERTIES OBJECT_DEPENDS "${gen}/${exe_stem}.manifest")
    target_include_directories(${id} PRIVATE "${gen}")
    target_compile_definitions(${id} PRIVATE NXRT_GAME_REGISTER=nxrt_register_game_${id} NXRT_VERSION="${A_VERSION}" NXGAME_LOG="${G_LOG}"
                               NXGAME_EXEFS_DEFAULT="${exefs_default}" NXGAME_ROMFS_DEFAULT="${romfs_default}")
    target_link_libraries(${id} PRIVATE ${libs} nxrt_game_${id} nxrt_gfx nxrt)
    set_target_properties(${id} PROPERTIES OUTPUT_NAME "${exe_stem}" RUNTIME_OUTPUT_DIRECTORY "${out}")
    # A linker map next to the exe (host frames get names), a big main stack (lifted code nests deeply), low host allocations so the guest
    # ranges at 0x71_0000_0000 and up stay free.
    if(NXRT_RELEASE)
        target_compile_definitions(${id} PRIVATE NXGAME_GUI_SUBSYSTEM=1)
        target_link_options(${id} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /PDBALTPATH:${exe_stem}.pdb)
    endif()
    target_link_options(${id} PRIVATE /MAP /STACK:134217728 /HIGHENTROPYVA:NO /DEBUG /MANIFEST:NO)   # the .rc carries the manifest
    # The ASTC decoder DLLs astc2bc7 loads at run time (NVN textures in ASTC are transcoded to BC7 on first use), and the shared-font
    # fallback (src/services/pl.cpp looks in fonts\ next to the exe when the dump has no shared font).
    foreach(dll ${NXRT_A2B_DLLS})
        add_custom_command(TARGET ${id} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:${id}>")
    endforeach()
    add_custom_command(TARGET ${id} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NXRT_SOURCE_DIR}/assets/fonts/DejaVuSans.ttf" "$<TARGET_FILE_DIR:${id}>/fonts/DejaVuSans.ttf")

    # A game with launcher pages of its own (nxrt/game/launcher_ui.h GameLauncher) has a library nxrt_game_<id>_launcher with its
    # register function nxrt_register_game_<id>_launcher; the others brand the launcher with the data library alone.
    if(TARGET nxrt_game_${id}_launcher)
        set(_launcher_lib nxrt_game_${id}_launcher)
        set(_launcher_reg nxrt_register_game_${id}_launcher)
    else()
        set(_launcher_lib nxrt_game_${id}_data)
        set(_launcher_reg nxrt_register_game_${id}_data)
    endif()
    nxrt_add_launcher(${id}_launcher VERSION "${A_VERSION}" GAME_LIB ${_launcher_lib} REGISTER ${_launcher_reg}
                      RC "${gen}/${launcher_stem}.rc" OUTPUT_NAME "${launcher_stem}" OUTPUT_DIR "${out}")
    set_source_files_properties("${gen}/${launcher_stem}.rc" PROPERTIES OBJECT_DEPENDS "${gen}/${launcher_stem}.manifest")
endfunction()
