// SPDX-License-Identifier: GPL-3.0-or-later
// Launcher helpers: what is in the game folder, the launcher's own settings file, command-line quoting, and starting mpr.exe
// or the setup tool. The pure parts (check_game_dir, summary, parse/format, quote_arg) do not touch the registry or the
// launcher's own folder.
#pragma once
#include <windows.h>

#include <string>

namespace mprl {

struct GameStatus {
    bool exefs = false;           // exefs\main exists
    bool romfs = false;           // romfs\ exists and holds at least one entry
    bool textures = false;        // bc7_cache\ exists and holds at least one entry
    bool shaders = false;         // shader_cache\ exists and holds at least one entry
    bool setup_finished = false;  // setup_state.tsv records a finished setup run
    bool ready() const;           // exefs and romfs are there, enough to start the game
    std::wstring summary() const; // one plain sentence for the player
};

// Pure filesystem checks of one game folder. Never throws; an empty dir gives nothing found.
GameStatus check_game_dir(const std::wstring& dir);

// Folder of the running launcher exe (no trailing backslash), and the default game folder (the same folder).
std::wstring launcher_exe_dir();
std::wstring default_game_dir();
std::wstring controls_path(const std::wstring& game_dir);  // <dir>\controls.ini
std::wstring launcher_config_path();                       // <exe dir>\mpr_launcher.ini

// The launcher's own settings file: UTF-8 text with a [launcher] section.
//   [launcher]
//   game_dir = D:\Games\Metroid Prime
struct LauncherConfig {
    std::wstring game_dir;
};
std::string format_launcher_config(const LauncherConfig& cfg);
// Fills *out. Keys that are missing keep the value already in *out. Returns false (and leaves *out alone) when
// the value is not valid UTF-8 or out is null.
bool parse_launcher_config(const std::string& utf8_text, LauncherConfig* out);
// On failure (missing or unreadable file, bad text) *out gets the default game folder and false is returned.
bool load_launcher_config(const std::wstring& path, LauncherConfig* out);
// Writes a temp file next to the target and renames it over the target.
bool save_launcher_config(const std::wstring& path, const LauncherConfig& cfg, std::wstring* err);

// One command-line argument, quoted by the CommandLineToArgvW rules. Plain words stay as they are.
std::wstring quote_arg(const std::wstring& s);

// Starts <launcher exe dir>\mpr.exe --game <game_dir> [extra_args] with the launcher folder as working directory.
// Does not wait. extra_args is added as typed (for test runs).
bool start_game(const std::wstring& game_dir, std::wstring* err, const std::wstring& extra_args = L"");

// Starts <launcher exe dir>\mpr_setup.exe --out <game_dir> [extra_args]. The process handle goes to *process;
// the caller waits for it and closes it. *process is null when false is returned.
bool start_setup(const std::wstring& game_dir, HANDLE* process, std::wstring* err, const std::wstring& extra_args = L"");

}  // namespace mprl
