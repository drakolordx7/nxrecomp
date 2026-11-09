# SPDX-License-Identifier: GPL-3.0-or-later
#
# nxrecomp_add_game(DIR <games/<id>> [EXEFS <dir>] [GEN <dir>] [JOBS <n>])
#   A game's lifted libraries from its manifest (games/<id>/game.json, base cmake/nx_game.cmake): one nxrecomp_add_module per module,
#   named <id>_<module>, with the module's index (its position in the manifest), base, shard count and site list, and the lifter config
#   games/<id>/recomp/config.py. Also <id>_modules (all of them) and <id>_link_check (EXCLUDE_FROM_ALL: the libraries plus the runtime
#   stubs of testing/rt_stubs.cpp; proves every symbol resolves). EXEFS default <dir>/game/exefs, GEN (generated C++) default <dir>/work/lift.
#
# nxrecomp_add_module(<target>
#     INPUT <module file>          NSO or static ELF (see --kind)
#     NAME <module name>           default: main        (the generated descriptor is `nxmod_<NAME>`)
#     [BASE <0x...>]               guest address the module is mapped at (default 0x7100000000)
#     [INDEX <n>]                  NxModule.index / import-id module number (default 0; nxrecomp_add_game passes the module's position in the manifest)
#     [GAME <config.py>]           the game's lifter config, games/<id>/recomp/config.py (default: generic)
#     [KIND nso|nx_elf|static_elf] default: guessed from the file's magic
#     [SHARDS <n>]                 exactly n translation units c_000.cpp ... (default 16)
#     [JOBS <n>]                   lifter processes (default: all cores)
#     [SITES <file>]               site list: instruction patches, inline callouts, function hooks (nxrecomp/program.py load_sites)
#     [OUTDIR <dir>])              where the generated sources go (default: <build>/gen/<target>)
#
# Lifts the module with `python -m nxrecomp lift` (a build step, re-run when the input or the lifter changes) and builds
# the result as the static library <target>. Link it into the runtime: it provides `nxmod_<NAME>` (ctx.h: NxModule) and
# expects the runtime to provide the functions ctx.h declares as runtime-provided. Public include directories: recomp/include
# (nxrecomp/ctx.h) and the generated folder (funcs.h).
#
# Flags: clang-cl, C++20, no exceptions/RTTI, -fno-strict-aliasing and -ffp-contract=off (smo-recomp's: lifted FP must not be
# fused or reordered), -mcx16 (128-bit exclusives are cmpxchg16b), /O1. Functions longer than OPTNONE_INSNS (program.py) are
# emitted with NX_OPTNONE by the lifter itself (bounded compile time).

set(NXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "recomp/ folder")
include("${CMAKE_CURRENT_LIST_DIR}/nx_toolchain.cmake")     # NX_PYTHON

file(GLOB NX_LIFTER_SOURCES CONFIGURE_DEPENDS
     "${NXRECOMP_ROOT}/nxrecomp/*.py" "${NXRECOMP_ROOT}/nxrecomp/lift/*.py" "${NXRECOMP_ROOT}/nxrecomp/lift/tier2/*.py"
     "${NXRECOMP_ROOT}/include/nxrecomp/*.h")

function(nxrecomp_add_module target)
  cmake_parse_arguments(A "" "INPUT;NAME;BASE;INDEX;GAME;KIND;SHARDS;JOBS;OUTDIR;SITES" "" ${ARGN})
  if(NOT A_NAME)
    set(A_NAME main)
  endif()
  if(NOT A_BASE)
    set(A_BASE 0x7100000000)
  endif()
  if(NOT A_SHARDS)
    set(A_SHARDS 16)
  endif()
  if(NOT A_JOBS)
    cmake_host_system_information(RESULT A_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
  endif()
  if(NOT A_OUTDIR)
    set(A_OUTDIR "${CMAKE_CURRENT_BINARY_DIR}/gen/${target}")
  endif()
  set(extra "")
  set(game_deps "")
  if(A_GAME)
    list(APPEND extra --game ${A_GAME})
    get_filename_component(game_cfg_dir "${A_GAME}" DIRECTORY)
    file(GLOB game_deps CONFIGURE_DEPENDS "${game_cfg_dir}/*.py")
  endif()
  if(A_KIND)
    list(APPEND extra --kind ${A_KIND})
  endif()
  if(NOT "${A_INDEX}" STREQUAL "")
    list(APPEND extra --index ${A_INDEX})
  endif()
  if(A_SITES)
    list(APPEND extra --sites ${A_SITES})
  endif()
  set(outs "${A_OUTDIR}/funcs.h" "${A_OUTDIR}/module_${A_NAME}.cpp")
  set(shard_srcs "")
  math(EXPR last "${A_SHARDS} - 1")
  foreach(i RANGE ${last})
    string(LENGTH "${i}" len)
    if(len EQUAL 1)
      set(n "00${i}")
    elseif(len EQUAL 2)
      set(n "0${i}")
    else()
      set(n "${i}")
    endif()
    list(APPEND shard_srcs "${A_OUTDIR}/c_${n}.cpp")
  endforeach()
  add_custom_command(
    OUTPUT ${outs} ${shard_srcs}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${A_OUTDIR}"
    COMMAND "${NX_PYTHON}" -m nxrecomp lift "${A_INPUT}" "${A_OUTDIR}" --name ${A_NAME} --base ${A_BASE}
            --shards ${A_SHARDS} --jobs ${A_JOBS} ${extra}
    WORKING_DIRECTORY "${NXRECOMP_ROOT}"
    DEPENDS "${A_INPUT}" ${A_SITES} ${NX_LIFTER_SOURCES} ${game_deps}
    COMMENT "nxrecomp: lifting ${A_NAME} (${A_INPUT})"
    VERBATIM)
  add_library(${target} STATIC ${shard_srcs} "${A_OUTDIR}/module_${A_NAME}.cpp")
  target_include_directories(${target} PUBLIC "${NXRECOMP_ROOT}/include" "${A_OUTDIR}")
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_compile_options(${target} PRIVATE /O1 /w /EHs-c- /GR- -fno-strict-aliasing -ffp-contract=off -mcx16)   # clang-cl
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/nx_game.cmake")

function(nxrecomp_add_game)
  cmake_parse_arguments(A "" "DIR;EXEFS;GEN;JOBS" "" ${ARGN})
  nx_game_read("${A_DIR}" G)
  if(NOT A_EXEFS)
    set(A_EXEFS "${G_DIR}/game/exefs")
  endif()
  if(NOT A_GEN)
    set(A_GEN "${G_DIR}/work/lift")
  endif()
  set(cfg "${G_DIR}/recomp/config.py")
  set(game_arg "")
  if(EXISTS "${cfg}")
    set(game_arg GAME "${cfg}")
  endif()
  set(jobs_arg "")
  if(A_JOBS)
    set(jobs_arg JOBS ${A_JOBS})
  endif()
  set(libs "")
  set(inc "")
  foreach(m ${G_MODULES})
    set(sites_arg "")
    if(G_MODULE_${m}_SITES)
      set(sites_arg SITES "${G_MODULE_${m}_SITES}")
    endif()
    set(shards ${G_MODULE_${m}_SHARDS})
    if(NOT shards)
      set(shards 16)
    endif()
    nxrecomp_add_module(${G_ID}_${m} INPUT "${A_EXEFS}/${G_MODULE_${m}_FILE}" NAME ${m} INDEX ${G_MODULE_${m}_INDEX}
                        BASE ${G_MODULE_${m}_BASE} SHARDS ${shards} OUTDIR "${A_GEN}/${m}" ${game_arg} ${sites_arg} ${jobs_arg})
    list(APPEND libs ${G_ID}_${m})
    string(APPEND inc "NXGAME_MODULE(${m})
")
  endforeach()
  add_custom_target(${G_ID}_modules DEPENDS ${libs})
  set(gen "${CMAKE_CURRENT_BINARY_DIR}/${G_ID}_link_check")
  file(CONFIGURE OUTPUT "${gen}/link_check_modules.inc" CONTENT "${inc}")
  add_executable(${G_ID}_link_check EXCLUDE_FROM_ALL "${NXRECOMP_ROOT}/testing/link_check.cpp" "${NXRECOMP_ROOT}/testing/rt_stubs.cpp")
  target_include_directories(${G_ID}_link_check PRIVATE "${gen}")
  target_link_libraries(${G_ID}_link_check PRIVATE ${libs})
  target_compile_options(${G_ID}_link_check PRIVATE /w /EHs-c- /GR-)
endfunction()
