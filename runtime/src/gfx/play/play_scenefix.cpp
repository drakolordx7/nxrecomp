// SPDX-License-Identifier: GPL-3.0-or-later
// Guards for latent races in the game's own scene code (S42). See play_scenefix.h.
#include "play/play_scenefix.h"

#include <atomic>
#include <cstring>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"

namespace nxrt::play {
namespace {
constexpr const char* kTag = "scenefix";
constexpr uint64_t kFnModConProcessRenderQuery = 0x1b57ac;       // CModConSceneNode::ProcessRenderQuery(CSceneRenderQuery const&, CSceneRenderQueryResult&)
constexpr uint64_t kFnModConMP1ProcessRenderQuery = 0xfba8b8;    // CModConSceneNodeMP1::ProcessRenderQuery (same, plus the draw flags at +0x1d0)
constexpr uint64_t kListedByte = 0x31;       // CRenderSceneNode: bit 0 = in a render list (PullTransferData)
constexpr uint64_t kReadyByte = 0x32;        // CModConSceneNode: bit 0 = tokens ready, render data built (UpdateTokenReadiness)
constexpr uint64_t kRenderData = 0x80;       // CModConSceneNode: NModConFormat::CModConRenderData*, the bounds at +0x190 (GetBounds)

struct State {
    uint32_t ids[2] = {0, 0};
    std::atomic<uint64_t> skipped{0};
};
State g_s;

int query_hook(::Ctx* c, uint64_t, void*) {
    const uint8_t* node = reinterpret_cast<const uint8_t*>(c->x[0]);
    if (!node || !modcon_query_must_skip(node)) return 0;           // the game's function runs as it is
    // Handled: the lifted function returns at once with x0 = the node, as the original does on its "not listed" path (the callers ignore it).
    const uint64_t n = g_s.skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) == 0)        // 1, 2, 4, 8, ...
        NXLOG_I(kTag, "render query skipped a ModCon scene node that is listed but not loaded yet (node 0x%llx, %llu so far)",
                static_cast<unsigned long long>(c->x[0]), static_cast<unsigned long long>(n));
    return 1;
}
}  // namespace

bool modcon_query_must_skip(const uint8_t* node) {
    if ((node[kListedByte] & 1) == 0) return false;                 // not listed: the original returns at once by itself
    uint64_t data = 0;
    std::memcpy(&data, node + kRenderData, sizeof data);
    return (node[kReadyByte] & 1) == 0 || data == 0;
}

void scenefix_install(uint64_t game_base) {
    if (g_s.ids[0] || g_s.ids[1]) return;
    if (!config_get_bool("game", "modcon_query_guard", true)) {
        NXLOG_I(kTag, "ModCon render query guard off ([game] modcon_query_guard = 0)");
        return;
    }
    g_s.ids[0] = hook_add_entry(game_base + kFnModConProcessRenderQuery, &query_hook);
    g_s.ids[1] = hook_add_entry(game_base + kFnModConMP1ProcessRenderQuery, &query_hook);
}

void scenefix_remove() {
    for (uint32_t& id : g_s.ids) {
        if (id) hook_remove(id);
        id = 0;
    }
}

uint64_t scenefix_modcon_skipped() { return g_s.skipped.load(); }

}  // namespace nxrt::play
