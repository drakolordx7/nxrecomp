// SPDX-License-Identifier: GPL-3.0-or-later
// S42: the ModCon render query guard (games/mpr/play_scenefix.cpp), driven the way lifted code drives it: nx_hook_entry on the guest address of
// CModConSceneNode::ProcessRenderQuery and of the CModConSceneNodeMP1 override, with x0 = a fake scene node in host memory.
#include <cstdint>
#include <cstring>
#include <vector>

#include "test_framework.h"

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "mpr/play_scenefix.h"

NXTEST_GROUP(play_scenefix)

namespace {

namespace play = nxrt::play;

constexpr uint64_t kBase = 0x5B00000000ull;
constexpr uint64_t kBaseQuery = 0x1b57ac, kMp1Query = 0xfba8b8;

// A CModConSceneNodeMP1 as the queries see it: +0x31 bit 0 listed, +0x32 bit 0 tokens ready, +0x80 render data (bounds at +0x190).
struct Node {
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x1e0);
    alignas(16) uint8_t data[0x400] = {};
    void set(bool listed, bool ready, bool has_data) {
        mem[0x31] = static_cast<uint8_t>(listed ? 0x81 : 0x80);      // other bits of the byte are left set: only bit 0 counts
        mem[0x32] = static_cast<uint8_t>(ready ? 0x05 : 0x04);
        const uint64_t p = has_data ? reinterpret_cast<uint64_t>(data) : 0;
        std::memcpy(mem.data() + 0x80, &p, 8);
    }
    uint64_t addr() const { return reinterpret_cast<uint64_t>(mem.data()); }
};

int call(uint64_t fn, const Node& n) {
    ::Ctx c{};
    c.x[0] = n.addr();
    c.x[1] = 0x1111;
    c.x[2] = 0x2222;
    const int r = nx_hook_entry(&c, kBase + fn);
    CHECK_EQ(c.x[0], n.addr());           // a skip returns the way the original's "not listed" path does: x0 is still the node
    CHECK_EQ(c.x[1], 0x1111u);
    return r;
}

}  // namespace

NXTEST(rule_skips_only_a_listed_node_without_its_data) {
    Node n;
    n.set(false, false, false);
    CHECK(!play::modcon_query_must_skip(n.mem.data()));        // not listed: the game's own test returns at once
    n.set(true, true, true);
    CHECK(!play::modcon_query_must_skip(n.mem.data()));        // listed and loaded: the query reads a real box
    n.set(true, false, false);
    CHECK(play::modcon_query_must_skip(n.mem.data()));         // listed, tokens not ready: the box pointer is null (the crash)
    n.set(true, false, true);
    CHECK(play::modcon_query_must_skip(n.mem.data()));         // listed, data from an earlier share but not ready: the draw path skips it too
    n.set(true, true, false);
    CHECK(play::modcon_query_must_skip(n.mem.data()));         // never seen; skipped rather than read through null
}

NXTEST(hooks_skip_the_unloaded_node_on_both_functions) {
    nxrt::hooks_clear();
    nxrt::config_clear();
    play::scenefix_install(kBase);
    Node n;
    const uint64_t before = play::scenefix_modcon_skipped();
    n.set(true, false, false);
    CHECK_EQ(call(kBaseQuery, n), 1);
    CHECK_EQ(call(kMp1Query, n), 1);
    CHECK_EQ(play::scenefix_modcon_skipped(), before + 2);
    n.set(true, true, true);
    CHECK_EQ(call(kBaseQuery, n), 0);                          // the game's function runs
    CHECK_EQ(call(kMp1Query, n), 0);
    n.set(false, false, false);
    CHECK_EQ(call(kMp1Query, n), 0);
    CHECK_EQ(play::scenefix_modcon_skipped(), before + 2);
    play::scenefix_remove();
    n.set(true, false, false);
    CHECK_EQ(call(kMp1Query, n), 0);                           // removed: nothing intercepts
    nxrt::hooks_clear();
}

NXTEST(config_off_leaves_the_games_code_alone) {
    nxrt::hooks_clear();
    nxrt::config_clear();
    nxrt::config_set("game", "modcon_query_guard", "0");
    play::scenefix_install(kBase);
    Node n;
    n.set(true, false, false);
    CHECK_EQ(call(kMp1Query, n), 0);
    CHECK_EQ(nxrt::hook_count_at(kBase + kMp1Query, nxrt::HookKind::Entry), 0u);
    play::scenefix_remove();
    nxrt::config_clear();
    nxrt::hooks_clear();
}
