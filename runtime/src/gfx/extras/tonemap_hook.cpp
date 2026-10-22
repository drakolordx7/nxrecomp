// SPDX-License-Identifier: GPL-3.0-or-later
#include "extras/tonemap_hook.h"

#include <atomic>
#include <cstring>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/extras.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "extras/hdr_math.h"

namespace nxrt::extras {
namespace {
constexpr const char* kTag = "hdr";
constexpr uint64_t kFnBuildEvalParams = 0x211c60;
std::atomic<uint32_t> g_hook_id{0};
std::atomic<uint32_t> g_calls{0};
thread_local bool t_nested = false;

int hook(::Ctx* c, uint64_t addr, void*) {
    if (t_nested) return 0;                                  // the call that runs the game's own function
    const HdrState hs = hdr_state();
    if (!hs.scene_hdr || hs.peak_ratio <= 1.001f) return 0;
    const uint64_t out = c->x[8];                            // sret: the 80-byte block
    if (!out || !mem::is_mapped(out, 80)) return 0;
    ::Ctx saved = *c;
    t_nested = true;
    dispatch_call(c, addr);
    t_nested = false;
    const auto s0 = c->v[0];                                 // the function's (meaningless) float result stays what the original left
    *c = saved;
    c->v[0] = s0;
    TonemapEval e;
    std::memcpy(e.v, reinterpret_cast<const void*>(out), sizeof e.v);
    if (extend_shoulder(&e, hs.peak_ratio)) {
        std::memcpy(reinterpret_cast<void*>(out + 48), &e.v[12], 4 * sizeof(float));      // shoulder A, p, q, B
        const uint32_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || n == 1000)
            NXLOG_I(kTag, "tone curve block %u: exposure %.4f, toe/linear end %.3f/%.3f, shoulder A %.4f p %.4f q %.4f B %.4f (peak ratio %.2f)", n, e.v[0], e.v[10], e.v[11], e.v[12], e.v[13],
                    e.v[14], e.v[15], hs.peak_ratio);
    }
    return 1;
}
}  // namespace

void tonemap_hook_install(uint64_t module_base) {
    if (g_hook_id.load()) return;
    g_hook_id.store(hook_add_entry(module_base + kFnBuildEvalParams, &hook));
}

void tonemap_hook_remove() {
    if (const uint32_t id = g_hook_id.exchange(0)) hook_remove(id);
}

uint32_t tonemap_hook_calls() { return g_calls.load(); }

}  // namespace nxrt::extras
