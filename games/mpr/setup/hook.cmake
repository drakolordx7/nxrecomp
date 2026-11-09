# SPDX-License-Identifier: GPL-3.0-or-later
# Metroid Prime Remastered's part of the setup tool, included by the base tools/setup/CMakeLists.txt (the manifest's "setup_hook"):
# the Retro readers (pak, TXTR, the shader program scan), the hook that hands them to the base installer, the real-data texture quality
# probe and their tests. The base provides NXS_SRC, BC7_TP, nxs_lib, nxs_test_in and the libraries named below.
set(_mpr_setup "${CMAKE_CURRENT_LIST_DIR}")

add_library(nxs_game_hook STATIC
    ${_mpr_setup}/pak.cpp ${_mpr_setup}/txtr.cpp ${_mpr_setup}/programs.cpp ${_mpr_setup}/mpr_setup.cpp)
target_include_directories(nxs_game_hook PUBLIC ${NXS_SRC} ${_mpr_setup})
target_compile_definitions(nxs_game_hook PUBLIC _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(nxs_game_hook PRIVATE ${NXS_WARN})
target_link_libraries(nxs_game_hook PUBLIC nxs_base nxs_crypto miniz_lib)

# Tests (tests/test_<name>.cpp). pak: the readers alone. programs, texcache: through the hook and the base cache writer.
nxs_test_in(${_mpr_setup}/tests pak nxs_game_hook nxs_base)
foreach(t programs texcache)
    nxs_test_in(${_mpr_setup}/tests ${t} nxs_core)
endforeach()

# nx_texq: real-data BC7 quality probe (texq.cpp, scripts/check_texquality.py); uses the oracle's reference BC7 decoder.
add_executable(nx_texq ${_mpr_setup}/texq.cpp)
target_include_directories(nx_texq PRIVATE ${NXS_SRC} ${BC7_TP} ${_mpr_setup})
target_link_libraries(nx_texq PRIVATE nxs_core)
target_compile_definitions(nx_texq PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
foreach(isa avx2 sse4.1 sse2)
    add_custom_command(TARGET nx_texq POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:astcdec-${isa}-shared> $<TARGET_FILE_DIR:nx_texq>)
endforeach()
