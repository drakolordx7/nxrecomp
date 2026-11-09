# SPDX-License-Identifier: GPL-3.0-or-later
# Where the build tools are: the one place that knows (scripts/nx_env.cmd sets the same environment variables for a shell).
#   NX_LLVM_BIN   LLVM's bin folder (clang-cl, lld-link, llvm-rc, llvm-mt): env LLVM_BIN, else clang-cl on PATH, else
#                 %ProgramFiles%\LLVM\bin
#   NX_PYTHON     Python with recomp/requirements.txt: env NX_PYTHON, else <base>/tools/.venv, else python on PATH
#   NX_SCCACHE    sccache (compiler cache), unless env NX_NO_SCCACHE is set: PATH, else the WinGet links folder
# Included by cmake/clang-cl.cmake (the toolchain file) and cmake/NxRecomp.cmake; safe to include more than once.
if(NOT NX_LLVM_BIN)
  if(DEFINED ENV{LLVM_BIN})
    file(TO_CMAKE_PATH "$ENV{LLVM_BIN}" NX_LLVM_BIN)
  else()
    find_program(_nx_clang_cl clang-cl)
    if(_nx_clang_cl)
      get_filename_component(NX_LLVM_BIN "${_nx_clang_cl}" DIRECTORY)
    else()
      file(TO_CMAKE_PATH "$ENV{ProgramFiles}/LLVM/bin" NX_LLVM_BIN)
    endif()
  endif()
  set(NX_LLVM_BIN "${NX_LLVM_BIN}" CACHE PATH "LLVM bin folder (clang-cl, lld-link)")
endif()

if(NOT NX_PYTHON)
  get_filename_component(_nx_venv "${CMAKE_CURRENT_LIST_DIR}/../../tools/.venv/Scripts/python.exe" ABSOLUTE)
  if(DEFINED ENV{NX_PYTHON})
    file(TO_CMAKE_PATH "$ENV{NX_PYTHON}" _nx_py)
  elseif(EXISTS "${_nx_venv}")
    set(_nx_py "${_nx_venv}")
  else()
    find_program(_nx_py NAMES python3 python)
  endif()
  set(NX_PYTHON "${_nx_py}" CACHE FILEPATH "Python with the nxrecomp requirements")
endif()

if(NOT DEFINED ENV{NX_NO_SCCACHE} AND NOT NX_SCCACHE)
  find_program(NX_SCCACHE sccache HINTS "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Links")
endif()
