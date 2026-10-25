// SPDX-License-Identifier: GPL-3.0-or-later
// The runtime's texture edit hook (nxrt/tex_edit.h) has one owner; Metroid Prime Remastered has two features that edit textures: the suit and
// arm cannon colour (mpr_tint.cpp, base colour maps) and the glow colour (play_glow.cpp, the glow ramps and glow maps, S44b). This module
// owns the hook and hands each texture to the first client that wants it. The clients' ids are kept apart (the client's slot in the top byte).
#pragma once
#include <cstdint>

#include "nxrt/tex_edit.h"

namespace nxrt::play {

enum class TexEditClient : uint8_t { Tint = 0, Glow = 1, Count };

// Sets (hooks != nullptr) or removes a client's hooks. The client's ids must be below 2^56. The hooks are copied. The runtime hook is installed
// with the first client and removed with the last. The caller then calls gfx::texture_edit_changed when its answers changed.
void texedit_set(TexEditClient client, const gfx::TexEditHooks* hooks);

// The dispatcher's own functions, for the tests.
namespace texedit {
uint64_t match(const gfx::TexEditReq& r, void* user);
uint64_t generation(void* user);
void edit(uint64_t id, uint32_t level, uint32_t w, uint32_t h, uint8_t* rgba, void* user);
}  // namespace texedit

}  // namespace nxrt::play
