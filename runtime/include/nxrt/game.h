// SPDX-License-Identifier: GPL-3.0-or-later
// The game boundary (docs/GAME_BOUNDARY.md): everything one title supplies to the game-independent base.
//
// The runtime, the launcher and the test tooling know no game. A game module (games/<id>/runtime/, a static library the game exe links)
// fills one Info and registers it with set_current() before anything reads it: before nxrt::init() in the game exe, at the top of the
// launcher's WinMain and of the test mains. Every part is optional. A null table or an empty string turns off what needs it (no hooks, no
// probes, neutral title facts), so a new title can start with the identity alone and add tables as its hooks are found.
//
// What each table is for, and where the Metroid Prime Remastered one lives:
//   title_defaults   services::TitleProfile defaults (main.npdm, control.nacp); ini keys still override them    games/mpr/runtime/mpr_core.cpp
//   timestep         the sites of the real-time frame step (nxrt/timestep.h, Sites)                           games/mpr/runtime/mpr_core.cpp, mpr_sites.h
//   probes           test-tooling game-state probes and script steps (nxrt/tt.h, GameProbes)                  games/mpr/runtime/mpr_tt_probe.cpp
//   play_defaults    factory controls.ini text and the bindings added by later revisions (play_types.h)       games/mpr/runtime/mpr_defaults.cpp
//   play             play-mode hooks: aim, camera, lock-on, display, menu pointer, F1 entries (play_game.h)     games/mpr/runtime/mpr_play.cpp
//   prompts          the game's button icon textures (play/prompt_resolve.h, PromptGame)                      games/mpr/runtime/mpr_prompts.cpp
//   settings         the game's own section of controls.ini and its F1 tab (nxrt/game/settings.h)             games/mpr/runtime/mpr_settings.cpp
//   launcher         the game's launcher pages (nxrt/game/launcher_ui.h, GameLauncher; the launcher exe only)  games/mpr/runtime/mpr_launcher.cpp
//   render           the tone-map shader and its HDR patch, the render-target rule, NVN sizes (nxrt/game_render.h)  games/mpr/runtime/mpr_render.cpp
// The registry is header-only, so programs that do not link the runtime library (the launcher) use it too.
#pragma once
#include <cstdint>
#include <string>

namespace nxrt {
namespace services { struct TitleProfile; namespace fs { class ModsIndexEdit; } }
namespace timestep { struct Sites; }
namespace tt { struct GameProbes; }
namespace play { struct GamePlay; struct GameDefaults; struct PromptGame; struct GameSection; }
}
namespace nxl { struct GameLauncher; }

namespace nxrt::game {

struct GameRender;

struct Info {
    // ---- identity ----
    const char* id = "";                    // short id, also the stem of the game's file names: "mpr"
    const char* title = "";                 // the game's own name: "Metroid Prime Remastered"
    const char* product = "";               // the PC port's name in window titles and dialogs: "Metroid Prime Remastered PC"
    const char* short_title = "";           // a short name for file headers (the launcher's ini comment): "Metroid Prime" (empty: product)
    uint64_t program_id = 0;                // the Switch title id
    const char* version = "";               // the game version the hooks were found in: "1.0.0"
    uint32_t languages = 0;                 // control.nacp SupportedLanguage: bit n = services::Language n (0: American English only)

    // ---- files and branding ----
    const char* exe_name = "";              // the game exe: "mpr.exe"
    const char* setup_exe_name = "";        // the setup tool: "mpr_setup.exe"
    const char* launcher_ini = "";          // the launcher's own settings file: "mpr_launcher.ini"
    const char* game_ini = "";              // the settings file the setup tool writes and the game exe reads: "mpr.ini"
    const char* shader_pack = "";           // the shader pack file next to the setup tool: "shader_cache.mprshc"
    const char* save_folder = "";           // save data folder under %APPDATA%: "MPRRecomp" (empty: "nxrt")

    // ---- tables (null = the feature is off) ----
    void (*title_defaults)(services::TitleProfile& p) = nullptr;
    const timestep::Sites* timestep = nullptr;
    const tt::GameProbes* probes = nullptr;
    const play::GameDefaults* play_defaults = nullptr;
    const play::GamePlay* play = nullptr;
    const play::PromptGame* prompts = nullptr;
    const play::GameSection* settings = nullptr;
    const nxl::GameLauncher* launcher = nullptr;
    const GameRender* render = nullptr;
    // Called once the RomFS mods are merged, before they are served: fixes up game tables that depend on the served files
    // (src/services/fs_mods.h ModsIndexEdit). Not called when no mod provides anything.
    void (*mods_index_hook)(services::fs::ModsIndexEdit& edit) = nullptr;

    // ---- start-up (the generic game exe, runtime/apps/game) ----
    // Settings defaults as ini text, layered below the game ini and the environment (config.h config_set_defaults). nxrt::init() applies
    // them, and they stay across later ini loads. For per-title service switches ([net], [acc], ...).
    const char* config_defaults = nullptr;
    // Called once the game's modules are mapped and their lifted tables registered, before they are linked and before the first guest
    // instruction: hooks, patches of guest data, diagnostics. Returns false with a message in `error` to stop the start.
    bool (*boot)(std::string& error) = nullptr;
};

// The registered game, or nullptr. One slot per program (the function-local static is shared by every translation unit that inlines it).
inline const Info*& current_slot() {
    static const Info* slot = nullptr;
    return slot;
}
inline void set_current(const Info* g) { current_slot() = g; }
inline const Info* current() { return current_slot(); }
inline const Info& info() {
    static const Info none;
    const Info* g = current();
    return g ? *g : none;
}

}
