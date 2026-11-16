# SPDX-License-Identifier: GPL-3.0-or-later
# FFmpeg for the movie:: library (src/media/movie_ffmpeg.cpp): libavformat, libavcodec, libavutil, libswresample, libswscale, linked dynamically
# (LGPL-2.1+: the libraries stay separate files next to the exe, see THIRD_PARTY_NOTICES.md).
#
#   Windows   the pinned BtbN "lgpl-shared" build (x64, n8.1.3, the monthly autobuild of 2026-10-31, which the project keeps), fetched at configure time
#             into the archive cache NXRT_DEPS_DIR and checked against its SHA256: headers, import libraries, DLLs. The DLLs are copied next
#             to every exe at build time (nxrt_ffmpeg_deploy).
#   Linux     the system development packages (pkg-config: libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev,
#             FFmpeg 4.4 or newer; the code compiles against 4.4 .. 9.0). A package build links the ones of the distribution.
#             -DNXRT_FFMPEG_BUNDLED=ON uses the pinned BtbN linux64 build instead, the way a release does: its .so files are copied next to the exe
#             and found through an $ORIGIN rpath, so the release runs on any distribution and does not depend on the soname the distribution ships.
#   -DNXRT_FFMPEG_DIR=<prefix with include/ lib/ bin/>   any other FFmpeg build (a local one); wins over the above.
#
# Provides the target nxrt_ffmpeg (compile and link settings) and nxrt_ffmpeg_deploy(<exe target>) (the run-time files next to the exe).
include_guard(GLOBAL)
set(NXRT_FFMPEG_DIR "" CACHE PATH "Prefix of an FFmpeg build (include/, lib/, bin/); empty: the pinned download (Windows, or Linux with NXRT_FFMPEG_BUNDLED) or the system libraries (Linux)")
option(NXRT_FFMPEG_BUNDLED "Linux: use the pinned FFmpeg build instead of the system libraries (release builds)" OFF)

set(_nxff_libs avformat avcodec avutil swresample swscale)
set(_nxff_runtime "")

if(NOT NXRT_FFMPEG_DIR AND NX_PLATFORM STREQUAL "linux" AND NOT NXRT_FFMPEG_BUNDLED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NXRT_FFMPEG_PC REQUIRED IMPORTED_TARGET libavformat>=58 libavcodec>=58 libavutil>=56 libswresample>=3 libswscale>=5)
    add_library(nxrt_ffmpeg INTERFACE)
    target_link_libraries(nxrt_ffmpeg INTERFACE PkgConfig::NXRT_FFMPEG_PC)
    message(STATUS "nxrt: FFmpeg from the system (libavcodec ${NXRT_FFMPEG_PC_libavcodec_VERSION})")
else()
    if(NXRT_FFMPEG_DIR)
        set(_nxff_prefix "${NXRT_FFMPEG_DIR}")
    else()
        include(FetchContent)
        set(NXRT_DEPS_DIR "${NX_CACHE_DIR}/deps" CACHE PATH "Where the pinned third-party sources are downloaded to")
        set(_nxff_tag "autobuild-2026-10-31-12-59")
        set(_nxff_ver "n8.1.3-27-g8dca1586f4")
        if(NX_PLATFORM STREQUAL "win32")
            set(_nxff_file "ffmpeg-${_nxff_ver}-win64-lgpl-shared-8.1.zip")
            set(_nxff_sha "5cef7bc48e513f3cee7c881e8b2999d835541edb1846c3e89443a67f8f8556ab")
        else()
            set(_nxff_file "ffmpeg-${_nxff_ver}-linux64-lgpl-shared-8.1.tar.xz")
            set(_nxff_sha "493e89a48da02b9a7d217b60f7f60c3c917c9bed73459d8a106e5e987da082ef")
        endif()
        set(FETCHCONTENT_QUIET OFF)
        set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)
        FetchContent_Declare(ffmpeg_dev
            URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/${_nxff_tag}/${_nxff_file}"
            DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
            URL_HASH SHA256=${_nxff_sha}
            SOURCE_SUBDIR none)
        FetchContent_MakeAvailable(ffmpeg_dev)
        set(_nxff_prefix "${ffmpeg_dev_SOURCE_DIR}")
    endif()
    if(NOT EXISTS "${_nxff_prefix}/include/libavcodec/avcodec.h")
        message(FATAL_ERROR "nxrt: no FFmpeg headers in ${_nxff_prefix}/include")
    endif()
    add_library(nxrt_ffmpeg INTERFACE)
    target_include_directories(nxrt_ffmpeg SYSTEM INTERFACE "${_nxff_prefix}/include")
    foreach(_l ${_nxff_libs})
        if(NX_PLATFORM STREQUAL "win32")
            target_link_libraries(nxrt_ffmpeg INTERFACE "${_nxff_prefix}/lib/${_l}.lib")
            file(GLOB _f "${_nxff_prefix}/bin/${_l}-*.dll")
        else()
            target_link_libraries(nxrt_ffmpeg INTERFACE "${_nxff_prefix}/lib/lib${_l}.so")
            file(GLOB _f "${_nxff_prefix}/lib/lib${_l}.so.[0-9]*")
            list(FILTER _f EXCLUDE REGEX "[.]so[.][0-9]+[.][0-9]+")   # the soname file only (the full-version file is the same content)
        endif()
        list(APPEND _nxff_runtime ${_f})
    endforeach()
    if(NOT _nxff_runtime)
        message(FATAL_ERROR "nxrt: the FFmpeg run-time libraries are not in ${_nxff_prefix}/bin (lib)")
    endif()
    message(STATUS "nxrt: FFmpeg from ${_nxff_prefix}")
endif()
set_property(GLOBAL PROPERTY NXRT_FFMPEG_RUNTIME_FILES "${_nxff_runtime}")

# The FFmpeg libraries next to an executable (nothing for the system libraries), found through an $ORIGIN rpath on Linux.
function(nxrt_ffmpeg_deploy target)
    get_property(_files GLOBAL PROPERTY NXRT_FFMPEG_RUNTIME_FILES)
    if(NOT _files)
        return()
    endif()
    foreach(_f ${_files})
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" "$<TARGET_FILE_DIR:${target}>")
    endforeach()
    if(NX_PLATFORM STREQUAL "linux")
        set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
    endif()
endfunction()
