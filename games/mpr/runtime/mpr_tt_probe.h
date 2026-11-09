// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's test-tooling probes and script steps (nxrt/tt.h GameProbes): the table the game module registers, the probe layout
// the tests point at host memory of the same shape, and the guest-side operations the steps (mpr_tt_steps.cpp) are built on.
#pragma once
#include <cstdint>
#include <string>

#include "nxrt/abi.h"
#include "nxrt/tt.h"

namespace nxrt::tt::mpr {

// Probe layout (main-relative offsets, see docs/S5.md); tests point these at host memory built with the same shape.
struct ProbeLayout {
    uint64_t game_base = 0;                // base of the main module; 0 = [test] game_base, else where the console maps main (0x7100000000)
    uint64_t game_state_global = 0;        // address of the gpGameState variable (a pointer to CGameStateMP1); 0 = game_base + 0x459cc70
    uint64_t state_manager = 0;            // CStateManager*, 0 = whatever the ProcessInput hook captured
    uint64_t player = 0;                   // CPlayerMP1*, 0 = derived from the state manager (GameLogic + 0x18) or the hook
    uint64_t game_globals = 0;             // address of the CGameGlobalObjects static (its +0 = CResourceFactory*); 0 = game_base + 0x4578710
};
void set_probe_layout(const ProbeLayout& l);
// The state-pointer capture, called from the ProcessInput hook (x0 = CPlayerMP1*, x2 = CStateManager&); tests call it directly.
void set_player_pointers(uint64_t player, uint64_t state_manager);

const GameProbes& mpr_game_probes();
const GameStepDef* mpr_game_steps();            // mpr_tt_steps.cpp: @warp @give @grant @call @objs @probe @rays @watch
size_t mpr_game_step_count();

namespace detail {
// The game's resource loader as the probe reads it (S42: test warps wait for an idle loader). Unknown: not readable (no game globals yet).
enum class LoaderState { Unknown, Busy, Idle };
LoaderState loader_state(uint32_t* pending);   // *pending (may be null) = the queued loads when readable
bool give_item(int item, int amount);          // amount and capacity of one item of CPlayerStateMP1
bool warp(::Ctx* c, const uint8_t world[16], const uint8_t area[16]);
bool grant(int64_t item, int32_t amount, int32_t capacity);        // item -1 = every upgrade
bool call_guest(::Ctx* c, uint64_t offset, const std::string& args, uint64_t* result, std::string* err);
bool dump_objects(const std::string& classes, const std::string& out_path, std::string* err);
bool probe_positions(::Ctx* c, const std::string& in_path, const std::string& out_path, std::string* err);
bool cast_rays(::Ctx* c, const std::string& in_path, const std::string& out_path, std::string* err);
bool watch_start(::Ctx* c, int what);          // 1 vel.y, 2 pos.y, 3 both; false when the player is not readable yet
void watch_stop();
void watch_flush(uint64_t frame);
}

}
