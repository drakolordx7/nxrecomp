# SPDX-License-Identifier: GPL-3.0-or-later
# nx_setup_storage: the byte-source layer of the setup tool (IStorage, file / sub-range / memory storage, nxs::Error, hex and UTF-8 helpers).
# Included by the setup tool (tools/setup/CMakeLists.txt) and by the runtime's gfx.cmake, so a game's runtime module that reads the player's
# own archives (a Retro pak reader, say) links this library instead of compiling the setup tool's sources. Public headers: storage.h, common.h.
if(NOT TARGET nx_setup_storage)
    add_library(nx_setup_storage STATIC "${CMAKE_CURRENT_LIST_DIR}/storage.cpp" "${CMAKE_CURRENT_LIST_DIR}/util.cpp")
    target_include_directories(nx_setup_storage PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(nx_setup_storage PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    target_compile_features(nx_setup_storage PUBLIC cxx_std_20)
endif()
