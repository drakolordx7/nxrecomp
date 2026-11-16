# SPDX-License-Identifier: GPL-3.0-or-later
# The one place that decides the platform. Every CMake file of the base (runtime, recomp, tools) that needs a compiler flag, a linker flag, a
# library name or a source folder that differs between Windows and Linux takes it from here, so a fix to a build rule is made once and applies
# to both. Include it BEFORE project() (it sets the MSVC runtime there); safe to include again.
#
#   NX_PLATFORM                 win32 | linux (an unsupported system is a configure error)
#   NX_OS_DIR                   win32 | posix: the subfolder of src/os/ whose sources this platform builds (nx_platform_os_sources)
#   NX_EXE_SUFFIX               ".exe" | ""
#   NX_STATIC_LIB_PREFIX/SUFFIX "" ".lib" | "lib" ".a"       (the names the recomp build gives the lifted libraries, cmake/game_exe.cmake imports them by)
#   NX_SHARED_LIB_PREFIX/SUFFIX "" ".dll" | "lib" ".so"
#   NX_VENV_PYTHON_REL          Scripts/python.exe | bin/python   (inside tools/.venv)
#   NX_CACHE_DIR                where downloaded sources are cached (%LOCALAPPDATA%/nxrt | $XDG_CACHE_HOME/nxrt)
#   NX_GUEST_COMPILE_OPTIONS    flags of the lifted (generated) code: /O1 -fno-strict-aliasing -ffp-contract=off -mcx16 ... in the compiler's own spelling
#   NX_GUEST_NOEH_OPTIONS       the same minus optimisation: for the small host programs that link the lifted libraries
#   NX_RT_COMPILE_OPTIONS       warnings of the runtime and the tools
#   NX_RT_COMPILE_DEFINITIONS   definitions every runtime target gets (the Windows header switches)
# Functions (after project()):
#   nx_platform_setup()                       global compile/link options of the platform (static C++ runtime, lld)
#   nx_platform_clang_option(<out> <flag>)    a clang flag in the spelling of the driver in use (clang-cl needs the -clang: prefix)
#   nx_platform_exe_link_options(<out> [STACK <bytes>] [LOW_VA])   the link options of an executable: linker map, big main stack, with LOW_VA
#                                             low host addresses (the guest ranges stay free; Windows); nothing on Linux (the guest main thread gets its stack from the code, not the linker)
#   nx_platform_os_sources(<out> <root>)      the sources of <root>/${NX_OS_DIR} (src/os/win32 | src/os/posix), none of the other platform's
#   nx_platform_static_lib_path(<out> <dir> <name>)       <dir>/<prefix><name><suffix>
#   nx_platform_target_system_libs(<target>)  the system libraries the runtime needs (psapi | threads, dl)
if(CMAKE_SYSTEM_NAME)
    set(_nx_sys "${CMAKE_SYSTEM_NAME}")
else()
    set(_nx_sys "${CMAKE_HOST_SYSTEM_NAME}")
endif()
if(_nx_sys STREQUAL "Windows")
    set(NX_PLATFORM win32)
    set(NX_OS_DIR win32)
elseif(_nx_sys STREQUAL "Linux")
    set(NX_PLATFORM linux)
    set(NX_OS_DIR posix)
else()
    message(FATAL_ERROR "nx_platform: unsupported system '${_nx_sys}' (Windows x64 and Linux x86-64 are supported)")
endif()

if(NX_PLATFORM STREQUAL "win32")
    set(NX_EXE_SUFFIX ".exe")
    set(NX_STATIC_LIB_PREFIX "")
    set(NX_STATIC_LIB_SUFFIX ".lib")
    set(NX_SHARED_LIB_PREFIX "")
    set(NX_SHARED_LIB_SUFFIX ".dll")
    set(NX_VENV_PYTHON_REL "Scripts/python.exe")
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/nxrt" NX_CACHE_DIR)
    # Static MSVC runtime (/MT): players need no Visual C++ Redistributable. Set before project() so every target, third-party code included,
    # uses it; the libraries a target links must agree (a mismatch is a link error).
    if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # clang-cl spelling. Lifted code: no exceptions/RTTI, -fno-strict-aliasing and -ffp-contract=off (lifted FP must not be fused or reordered),
    # -mcx16 (128-bit exclusives are cmpxchg16b), /O1; /w: generated code is not read.
    set(NX_GUEST_COMPILE_OPTIONS /O1 /w /EHs-c- /GR- -fno-strict-aliasing -ffp-contract=off -mcx16)
    set(NX_GUEST_NOEH_OPTIONS /w /EHs-c- /GR-)
    set(NX_RT_COMPILE_OPTIONS /W3 -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type-mismatch
                              -Wno-cast-function-type-strict -Wno-language-extension-token)
    set(NX_RT_COMPILE_DEFINITIONS NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
else()
    set(NX_EXE_SUFFIX "")
    set(NX_STATIC_LIB_PREFIX "lib")
    set(NX_STATIC_LIB_SUFFIX ".a")
    set(NX_SHARED_LIB_PREFIX "lib")
    set(NX_SHARED_LIB_SUFFIX ".so")
    set(NX_VENV_PYTHON_REL "bin/python")
    if(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
        set(NX_CACHE_DIR "$ENV{XDG_CACHE_HOME}/nxrt")
    else()
        set(NX_CACHE_DIR "$ENV{HOME}/.cache/nxrt")
    endif()
    # GNU spelling of the same flags (clang on Linux, driver `clang++`)
    set(NX_GUEST_COMPILE_OPTIONS -O1 -w -fno-exceptions -fno-rtti -fno-strict-aliasing -ffp-contract=off -mcx16)
    set(NX_GUEST_NOEH_OPTIONS -w -fno-exceptions -fno-rtti)
    # -fno-omit-frame-pointer: the crash reporter and the sampler walk the host stack (the lifted code does not get it: NX_GUEST_COMPILE_OPTIONS)
    set(NX_RT_COMPILE_OPTIONS -Wall -fno-omit-frame-pointer -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type-mismatch
                              -Wno-cast-function-type-strict -Wno-language-extension-token)
    set(NX_RT_COMPILE_DEFINITIONS "")
endif()

function(nx_platform_setup)
    # once per configure: a project that adds the runtime as a subdirectory inherits the options of its own call
    get_property(_done GLOBAL PROPERTY NX_PLATFORM_SETUP_DONE)
    if(_done)
        return()
    endif()
    set_property(GLOBAL PROPERTY NX_PLATFORM_SETUP_DONE TRUE)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "nx_platform: a 64-bit x86 target is required")
    endif()
    if(NX_PLATFORM STREQUAL "linux")
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "nx_platform: build with clang (the lifted code and the runtime use clang extensions); got ${CMAKE_CXX_COMPILER_ID}")
        endif()
        # lld (the lifted libraries are about 150 translation units each), and the C++ runtime linked statically: a player needs no libstdc++.
        if(NOT CMAKE_EXE_LINKER_FLAGS MATCHES "ld-path|fuse-ld")
            add_link_options(-fuse-ld=lld)
        endif()
        add_link_options(-static-libstdc++ -static-libgcc)
    endif()
endfunction()

function(nx_platform_clang_option out flag)
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(${out} "-clang:${flag}" PARENT_SCOPE)
    else()
        set(${out} "${flag}" PARENT_SCOPE)
    endif()
endfunction()

function(nx_platform_exe_link_options out)
    cmake_parse_arguments(A "LOW_VA" "STACK" "" ${ARGN})
    set(opts "")
    if(NX_PLATFORM STREQUAL "win32")
        # A linker map next to the exe (host frames get names), a big main stack (lifted code nests deeply), low host allocations so the
        # guest ranges at 0x71_0000_0000 and up stay free.
        list(APPEND opts /MAP)
        if(A_STACK)
            list(APPEND opts /STACK:${A_STACK})
        endif()
        if(A_LOW_VA)
            list(APPEND opts /HIGHENTROPYVA:NO)
        endif()
        list(APPEND opts /DEBUG)
    endif()
    set(${out} ${opts} PARENT_SCOPE)
endfunction()

function(nx_platform_os_sources out root)
    file(GLOB_RECURSE _s CONFIGURE_DEPENDS "${root}/${NX_OS_DIR}/*.cpp")
    set(${out} ${_s} PARENT_SCOPE)
endfunction()

function(nx_platform_static_lib_path out dir name)
    set(${out} "${dir}/${NX_STATIC_LIB_PREFIX}${name}${NX_STATIC_LIB_SUFFIX}" PARENT_SCOPE)
endfunction()

function(nx_platform_target_system_libs target)
    if(NX_PLATFORM STREQUAL "win32")
        target_link_libraries(${target} PUBLIC psapi)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(${target} PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    endif()
endfunction()
