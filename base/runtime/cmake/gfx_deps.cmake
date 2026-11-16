# SPDX-License-Identifier: GPL-3.0-or-later
# Pinned third-party sources for the graphics shell (S4b). Every archive is fetched by URL and checked against a
# SHA256. The archives are cached in NXRT_DEPS_DIR (shared by checkouts and worktrees, survives a clean build); the
# extracted sources and the SDL build live in the build directory. Licences and pins are listed in THIRD_PARTY_NOTICES.md and README.md.
include(${CMAKE_CURRENT_LIST_DIR}/sdl_dep.cmake)   # SDL3 (pinned there, also used by the audio output of nxrt)
# Vulkan-Headers vulkan-sdk-1.4.363.0 (Apache-2.0 OR MIT)
FetchContent_Declare(vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.zip
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=f4be95220ff0ee0b1c620301ff4fe9ae48592d71f4c30f2d5c571351a153ae81
    SOURCE_SUBDIR none)
# volk vulkan-sdk-1.4.363.0 (MIT)
FetchContent_Declare(volk
    URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.zip
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=889b90fdcbe9cee7b118339d640c6895b7be5c920d309c44b744692063275b3d
    SOURCE_SUBDIR none)
# VulkanMemoryAllocator v3.4.0 (MIT)
FetchContent_Declare(vma
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.zip
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=ce9a61ea5e72284ac9ccc152c214eef381ab57bfd7383f20232e5a652e3afda6
    SOURCE_SUBDIR none)
# Dear ImGui v1.92.9b (MIT)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip
    DOWNLOAD_DIR "${NXRT_DEPS_DIR}"
    URL_HASH SHA256=e1c46d676c2bcb7ced847ba27f50553e33a19db97b3cadaec7f8be64449139f8
    SOURCE_SUBDIR none)

FetchContent_MakeAvailable(vulkan_headers volk vma imgui)

# volk (loader) next to the Vulkan headers; the Vulkan headers come from the pin, not from the installed SDK.
add_library(nxrt_volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(nxrt_volk SYSTEM PUBLIC "${volk_SOURCE_DIR}" "${vulkan_headers_SOURCE_DIR}/include")
target_compile_definitions(nxrt_volk PUBLIC VK_NO_PROTOTYPES)
if(NX_PLATFORM STREQUAL "win32")
    target_compile_definitions(nxrt_volk PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()
# (Linux: surfaces come from SDL_Vulkan_CreateSurface, which needs no VK_USE_PLATFORM_* define)
set_target_properties(nxrt_volk PROPERTIES C_STANDARD 11)

# VMA is header only; one translation unit in src/gfx defines VMA_IMPLEMENTATION.
add_library(nxrt_vma INTERFACE)
target_include_directories(nxrt_vma SYSTEM INTERFACE "${vma_SOURCE_DIR}/include")

# Dear ImGui core + its Vulkan backend (the platform side is fed from the nxrt input queue, so no SDL backend).
add_library(nxrt_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp" "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(nxrt_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(nxrt_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(nxrt_imgui PUBLIC nxrt_volk)
