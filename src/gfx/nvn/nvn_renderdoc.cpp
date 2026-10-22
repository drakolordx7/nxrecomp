// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] renderdoc_frame = N (NXRT_NVN_RENDERDOC_FRAME): when the process runs under RenderDoc (renderdoccmd capture ...), the frame
// that is presented as number N is captured: StartFrameCapture before its submissions are recorded, EndFrameCapture after its present.
// The headless ring has no swapchain, so RenderDoc's own frame counting never fires; this marks the frame instead. The RenderDoc in-app
// API is used through its exported RENDERDOC_GetAPI (version 1.1.2: the first 22 entries of the table, indexed below).
#include <windows.h>

#include <string>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu {
namespace {

using GetApiFn = int(__cdecl*)(int version, void** out);
using StartFn = void(__cdecl*)(void* device, void* window);
using EndFn = unsigned(__cdecl*)(void* device, void* window);
using SetTemplateFn = void(__cdecl*)(const char*);

constexpr int kApi_1_1_2 = 10102;
constexpr int kSetCaptureFilePathTemplate = 11, kStartFrameCapture = 19, kEndFrameCapture = 21;

void** g_api = nullptr;
bool g_tried = false;
bool g_capturing = false;
int64_t g_frame = -2;

bool api() {
    if (g_tried) return g_api != nullptr;
    g_tried = true;
    HMODULE m = GetModuleHandleA("renderdoc.dll");
    if (!m) return false;
    auto get = reinterpret_cast<GetApiFn>(GetProcAddress(m, "RENDERDOC_GetAPI"));
    if (!get) return false;
    void* table = nullptr;
    if (get(kApi_1_1_2, &table) != 1 || !table) return false;
    g_api = static_cast<void**>(table);
    NXLOG_I("nvn.renderdoc", "RenderDoc in-app API found");
    return true;
}

}  // namespace

void renderdoc_start_if_due(uint64_t presents) {
    if (g_frame == -2) g_frame = nxrt::config_get_int("nvn", "renderdoc_frame", -1);
    if (g_frame < 0 || g_capturing || static_cast<int64_t>(presents) != g_frame || !api()) return;
    static const std::string path = nxrt::config_get_str("nvn", "renderdoc_path", "");
    if (!path.empty()) reinterpret_cast<SetTemplateFn>(g_api[kSetCaptureFilePathTemplate])(path.c_str());
    reinterpret_cast<StartFn>(g_api[kStartFrameCapture])(nullptr, nullptr);
    g_capturing = true;
    NXLOG_I("nvn.renderdoc", "frame capture started at present %llu", static_cast<unsigned long long>(presents));
}

void renderdoc_end_if_capturing() {
    if (!g_capturing || !api()) return;
    g_capturing = false;
    g_frame = -1;   // one capture per run
    const unsigned ok = reinterpret_cast<EndFn>(g_api[kEndFrameCapture])(nullptr, nullptr);
    NXLOG_I("nvn.renderdoc", "frame capture ended (%s)", ok ? "saved" : "failed");
}

}  // namespace nxrt::nvn::be::gpu
