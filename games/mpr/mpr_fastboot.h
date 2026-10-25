// SPDX-License-Identifier: GPL-3.0-or-later
// Start-up and menu tweaks of Metroid Prime Remastered (S46, docs/S46.md; the settings are play_fastboot.h, `[tweaks]` in controls.ini).
//
//   skip_intro       The opening cutscene of a new game (the gunship landing on the Frigate Orpheon, Exterior Docking Hangar) is skipped through the
//                    game's own cutscene skip, as if the skip button had been pressed the moment the game allows it:
//                    - CGameState::IsCinematicSkippable answers true in that area (the practice mod and the randomizer make every cutscene skippable
//                      the same way, through mCinematicForceSkippableOverride, which that function reads). The area's CinematicSkip special function
//                      then registers itself as the current skip object (CStateManagerGameLogicMP1 +2932) when the cutscene starts.
//                    - CCommandFrame::GetDigitalPressedInput answers "pressed" for the two calls that read the skip command: the one in
//                      CGameIOWinMP1::UserInput (legacy cinematics) and the one in CCameraManager::CheckForCinematicSkip (the newer camera
//                      cinematics). The game then fades out and runs SpecialSkipCinematic, which sends the skip object's script messages: the
//                      level designers' own jump to the state the end of the cutscene leaves (player, camera, music, layers).
//   skip_boot_logos  The title's 14 s intro movie (FMV 45d4dd7e-45af-40f6-b6ed-8b76c33c90fb, the logo forming) is ended with the movie's own
//                    CStreamedMovieGOC::Skip(immediate) as soon as it plays; its end messages start the title loop, as at the end of the movie.
//   fast_menus       The front end's state manager (title, main menu, save slots, options, extras) is updated with its time step times menu_speed:
//                    CFrontEndIOWin::TimerTick marks the call, NIOWinStateManagerUtils::UpdateStateManager scales the step it hands to
//                    CStateManager::Update. Every script timer, UI animation and fade of the front end runs that much faster. The movies have their
//                    own clock (CMovieList::Update) and the game play is a different IOWin, so neither changes.
// Hook sites: recomp games/mpr/main.sites, "S46" block. Without them (an older lift) nothing fires and the game runs as before.
#pragma once
#include <cstdint>

namespace nxrt::play {

struct Settings;

void fastboot_install(uint64_t game_base);
void fastboot_remove();
void fastboot_set(const Settings& s);

// What happened, for the log and the tests.
struct FastbootStats {
    uint64_t skip_presses = 0;        // skip presses answered for the opening cutscene
    uint64_t skippable_answers = 0;   // IsCinematicSkippable answered true for it
    uint64_t movies_skipped = 0;      // title intro movies ended
    uint64_t menu_ticks = 0;          // front-end updates run faster
};
FastbootStats fastboot_stats();

// The id test of the opening cutscene's area (Frigate Orpheon, Exterior Docking Hangar) and of the title intro movie, on the 16 bytes of a CObjectId
// as the game keeps it (the first three groups of the GUID little-endian). Exposed for the tests.
bool fastboot_is_intro_area(const uint8_t world[16], const uint8_t area[16]);
bool fastboot_is_title_intro_movie(const uint8_t id[16]);

}  // namespace nxrt::play
