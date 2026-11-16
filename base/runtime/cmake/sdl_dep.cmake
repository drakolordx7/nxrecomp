# SPDX-License-Identifier: GPL-3.0-or-later
# SDL3, built from source as a static library: the windows, input and Vulkan surfaces of the graphics shell (cmake/gfx.cmake) and the audio output
# of the services (src/services/audio_sdl.cpp), on every platform. Included by CMakeLists.txt before the nxrt library (so a build without the
# graphics shell has SDL too) and by gfx_deps.cmake. The archive is cached in NXRT_DEPS_DIR (shared by checkouts and worktrees, survives a clean
# build); the extracted source and the SDL build live in the build directory.
include_guard(GLOBAL)
include(FetchContent)
set(NXRT_DEPS_DIR "${NX_CACHE_DIR}/deps" CACHE PATH "Where the pinned third-party sources are downloaded to")
set(FETCHCONTENT_QUIET OFF)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)   # extracted files take the time of extraction

# SDL 3.4.18 (zlib licence), released 2026-10-02.
FetchContent_Declare(sdl3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL_DOCS ON CACHE BOOL "" FORCE)
set(SDL_VENDOR_INFO "nxrt" CACHE STRING "" FORCE)
FetchContent_MakeAvailable(sdl3)
