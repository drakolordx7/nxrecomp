// SPDX-License-Identifier: GPL-3.0-or-later
// Launcher helpers: what is in the game folder, the launcher's own settings file, and starting the game exe, the setup tool and the shader
// preparation. Same code on every system: paths and text are UTF-8, files and processes go through nxrt/os. The pure parts (check_game_dir,
// summary, parse/format, split_args) touch neither the launcher's own folder nor any process. The names of the game's programs and files are
// the registered game's (nxrt/game.h; the launcher registers the data part of its game module at start).
#pragma once
#include "nx_formats.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/os/process.h"

struct SDL_Process;

namespace nxl {

// ---- Branding: the registered game's names (nxrt/game.h Info) ---------------------------------------------------------------------
std::string product_name();      // the window title and the dialogs: "<Product> PC"
std::string game_exe_name();     // "<id>" plus the executable suffix of the system
std::string setup_exe_name();    // "<id>_setup" plus the suffix
std::string shader_pack_name();  // the shader pack next to the launcher: "shader_cache.<ext>"
// A text of the registered game (UTF-8), or `fallback` when the game gives none.
std::string game_text(const char* utf8, const char* fallback);

struct GameStatus {
    bool exefs = false;           // exefs/main exists
    bool romfs = false;           // romfs/ exists and holds at least one entry
    bool textures = false;        // bc7_cache/ exists and holds at least one entry
    bool shaders = false;         // shader_cache/ exists and holds at least one entry
    bool setup_finished = false;  // setup_state.tsv records a finished setup run
    bool ready() const;           // exefs and romfs are there, enough to start the game
    std::string summary() const;  // one plain sentence for the player
};

// Pure filesystem checks of one game folder. Never throws; an empty dir gives nothing found.
GameStatus check_game_dir(const std::string& dir);

// `a` and `b` with one separator between (the one `a` already uses, else the system's).
std::string join_path(const std::string& a, const std::string& b);

// Folder of the running launcher exe (no trailing separator), and the default game folder (the same folder).
std::string launcher_exe_dir();
std::string default_game_dir();
std::string controls_path(const std::string& game_dir);  // <dir>/controls.ini
std::string launcher_config_path();                      // <exe dir>/<the game's launcher ini> (<id>_launcher.ini)

// The launcher's own settings file: UTF-8 text with a [launcher] section.
//   [launcher]
//   game_dir = D:\Games\Some Game
struct LauncherConfig {
    std::string game_dir;
};
std::string format_launcher_config(const LauncherConfig& cfg);
// Fills *out. Keys that are missing keep the value already in *out. Returns false (and leaves *out alone) when
// the value is not valid UTF-8 or out is null.
bool parse_launcher_config(const std::string& utf8_text, LauncherConfig* out);
// On failure (missing or unreadable file, bad text) *out gets the default game folder and false is returned.
bool load_launcher_config(const std::string& path, LauncherConfig* out);
// Writes a temp file next to the target and renames it over the target.
bool save_launcher_config(const std::string& path, const LauncherConfig& cfg, std::string* err);

// Splits the text of --game-args / --setup-args into arguments: separated by spaces or tabs, "double quotes" group (and may hold spaces), a
// backslash before a double quote is that quote, other backslashes stay as they are (the rules a Windows program command line has).
std::vector<std::string> split_args(const std::string& text);

// Starts <launcher exe dir>/<game exe> --game <game_dir> [extra_args] with the launcher folder as the current folder. Does not wait; the game
// goes on when the launcher ends. extra_args is the text of --game-args (for test runs).
bool start_game(const std::string& game_dir, std::string* err, const std::string& extra_args = "");

// Starts <launcher exe dir>/<setup exe> --out <game_dir> [extra_args]. The process goes to *child; the caller polls it with wait(0) and ends
// with it. *child is invalid when false is returned.
bool start_setup(const std::string& game_dir, nxrt::os::process::Child* child, std::string* err, const std::string& extra_args = "");

// ---- Shader preparation ----------------------------------------------------------------------------------------------
// `<id> --prepare-shaders [--if-needed]`, run in the game folder, compiles the game's shaders ahead of play. The setup tool writes
// <game folder>/shader_cache/programs.bin (the list of programs); a finished run writes shader_cache/prepare.state.

// The "prepare:" lines and prepare.state are defined in formats/nx_formats.h (the game exe writes them, the launcher reads them).
using nxfmt::PrepareKind;
using nxfmt::PrepareLine;
PrepareLine parse_prepare_line(const std::string& line);

// True when shader_cache/programs.bin exists. Without it there is nothing to prepare (older game folders).
bool shader_prepare_available(const std::string& game_dir);
// True when shader_cache/prepare.state says complete: 1. It does not know about driver changes; --if-needed checks those.
bool shader_prepare_done(const std::string& game_dir);

// One prepare run: starts <id> --prepare-shaders [--if-needed] with the game folder as working folder and its output (stdout and stderr) read
// through a pipe (SDL's process API: the os layer's Child has no output capture). A worker thread reads and parses the lines and never touches
// the window; it calls `notify` (thread-safe, cheap: it wakes the UI loop) when the progress or the result changed and when the child has ended
// and its output is read. The UI thread then reads progress() / result() and, once finished() is true, calls finish() exactly once.
class PrepareRun {
public:
    PrepareRun() = default;
    ~PrepareRun();   // a run still going is cancelled and waited for
    PrepareRun(const PrepareRun&) = delete;
    PrepareRun& operator=(const PrepareRun&) = delete;

    // Returns false with *err set when the run could not be started (the game exe missing, the system refused).
    bool start(const std::string& game_dir, bool if_needed, std::function<void()> notify, std::string* err);
    bool active() const { return active_; }        // started and finish() not called yet
    bool finished() const { return done_.load(); } // the child has ended and its output is read (call finish())
    void cancel();                                  // kills this run's child only; the next run starts from zero
    bool cancel_requested() const { return cancel_requested_.load(); }
    PrepareLine progress() const;                   // last "prepare: <done> <total>" line (kind other before the first)
    PrepareLine result() const;                     // last ready, done or error line (kind other until one comes)
    // Joins the worker and releases the process. Returns the child's exit code (-1 when unknown).
    int finish();

private:
    void worker();
    void handle_line(const std::string& line);

    bool active_ = false;
    std::function<void()> notify_;
    SDL_Process* proc_ = nullptr;
    std::thread thread_;
    mutable std::mutex mu_;                         // progress_ and result_
    PrepareLine progress_;
    PrepareLine result_;
    std::atomic<bool> done_{false};
    std::atomic<bool> cancel_requested_{false};
    int exit_code_ = -1;                            // written by the worker, read after join
};

}  // namespace nxl
