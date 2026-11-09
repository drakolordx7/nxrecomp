// SPDX-License-Identifier: GPL-3.0-or-later
// Guards for latent races in the game's own scene code (S42, docs/S42.md).
//
// ModCon render queries. A CModConSceneNode joins its scene's render lists as soon as its proxy is visible (CRenderSceneNode::PullTransferData
// sets the "listed" bit 0 of byte +0x31), but its render data (+0x80, which holds the bounding box at +0x190) exists only once all of the node's
// tokens are loaded (CModConSceneNode::UpdateTokenReadiness sets the "ready" bit 0 of byte +0x32 and the pointer together). The normal draw path
// (PreRenderForContextConcurrent) tests the ready bit before it reads the box. CModConSceneNode::ProcessRenderQuery and its CModConSceneNodeMP1
// override, which the decal code calls through CScene::ProcessRenderQueryFromList (shadow decals, geometry decals), test only the listed bit and read
// *(node + 0x80) + 0x190: a node that is listed and not loaded yet gives CFrustumPlanes::BoxInFrustumPlanes the box address 0x190
// (the magmoor_workstation crash). On the console the window is a frame or two; here a busy or starved loader thread stretches it.
// The entry hooks skip such a node, which is what the query does for any node it does not match: nothing is added to the result.
//
//   [game] modcon_query_guard = 1     default on; 0 runs the game's code unchanged (for reproducing the crash)
//
// The hook sites must be in recomp games/mpr/main.sites (entryhook 0x71001b57ac 0x7100fba8b8); without them this does nothing.
#pragma once
#include <cstdint>

namespace nxrt::play {

// The rule: true when a ModCon scene node is in a render list but its render data is not there yet (node = guest pointer = host pointer).
bool modcon_query_must_skip(const uint8_t* node);

void scenefix_install(uint64_t game_base);      // reads [game] modcon_query_guard; idempotent
void scenefix_remove();
uint64_t scenefix_modcon_skipped();             // render queries skipped on a node that was not loaded yet

}  // namespace nxrt::play
