// SPDX-License-Identifier: GPL-3.0-or-later
#include "mpr_texedit.h"

#include <mutex>

namespace nxrt::play {
namespace {

constexpr int kClients = static_cast<int>(TexEditClient::Count);
constexpr int kSlotShift = 56;

struct State {
    std::mutex mu;
    gfx::TexEditHooks hooks[kClients];
    bool installed = false;
};
State& S() {
    static State s;
    return s;
}

bool client_hooks(int i, gfx::TexEditHooks& h) {
    std::lock_guard<std::mutex> lk(S().mu);
    h = S().hooks[i];
    return h.match != nullptr;
}

}  // namespace

namespace texedit {

uint64_t match(const gfx::TexEditReq& r, void*) {
    for (int i = 0; i < kClients; ++i) {
        gfx::TexEditHooks h;
        if (!client_hooks(i, h)) continue;
        const uint64_t id = h.match(r, h.user);
        if (id && id < (1ull << kSlotShift)) return id | (static_cast<uint64_t>(i + 1) << kSlotShift);
    }
    return 0;
}

uint64_t generation(void*) {
    // any client's change is a new generation: the sum of theirs (each only grows)
    uint64_t g = 0;
    for (int i = 0; i < kClients; ++i) {
        gfx::TexEditHooks h;
        if (client_hooks(i, h) && h.generation) g += h.generation(h.user);
    }
    return g;
}

void edit(uint64_t id, uint32_t level, uint32_t w, uint32_t h, uint8_t* rgba, void*) {
    const int i = static_cast<int>(id >> kSlotShift) - 1;
    if (i < 0 || i >= kClients) return;
    gfx::TexEditHooks hk;
    if (!client_hooks(i, hk) || !hk.edit) return;
    hk.edit(id & ((1ull << kSlotShift) - 1), level, w, h, rgba, hk.user);
}

}  // namespace texedit

void texedit_set(TexEditClient client, const gfx::TexEditHooks* hooks) {
    const int i = static_cast<int>(client);
    if (i < 0 || i >= kClients) return;
    bool install = false, uninstall = false;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        S().hooks[i] = hooks ? *hooks : gfx::TexEditHooks{};
        bool any = false;
        for (const gfx::TexEditHooks& h : S().hooks) any |= h.match != nullptr;
        if (any && !S().installed) install = S().installed = true;
        if (!any && S().installed) {
            uninstall = true;
            S().installed = false;
        }
    }
    if (uninstall) gfx::set_texture_edit(nullptr);   // the edited textures go back to the game's
    if (install) {
        static gfx::TexEditHooks h;
        h.match = &texedit::match;
        h.generation = &texedit::generation;
        h.edit = &texedit::edit;
        h.user = nullptr;
        gfx::set_texture_edit(&h);
    }
}

}  // namespace nxrt::play
