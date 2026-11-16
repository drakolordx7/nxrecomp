// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_info.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "nx_formats.h"
#include "nxrt/game.h"
#include "nxrt/os/fs.h"
#include "nxrt/strutil.h"

#ifndef NXRT_EXE_SUFFIX
#define NXRT_EXE_SUFFIX ""
#endif

namespace nxl {

namespace fs = nxrt::os::fs;
namespace proc = nxrt::os::process;

namespace {

void set_err(std::string* err, const std::string& msg) {
    if (err) *err = msg;
}

bool is_blank(char c) { return c == ' ' || c == '\t'; }

using nxrt::str::lower;
using nxrt::str::trim;

// Valid UTF-8 (no overlong forms, surrogates or values past U+10FFFF).
bool valid_utf8(const std::string& s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len;
        uint32_t cp;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

// Value part of a "key = value" line. Either "quoted" (kept as is, may hold ';' and '#') or plain.
// A ';' or '#' that is first or comes after a space or tab starts a comment.
std::string parse_value(const std::string& raw) {
    size_t i = 0;
    while (i < raw.size() && is_blank(raw[i])) ++i;
    if (i < raw.size() && raw[i] == '"') {
        const size_t close = raw.find('"', i + 1);
        if (close != std::string::npos) return raw.substr(i + 1, close - i - 1);
    }
    size_t end = raw.size();
    for (size_t j = i; j < raw.size(); ++j) {
        if ((raw[j] == ';' || raw[j] == '#') && (j == 0 || is_blank(raw[j - 1]))) {
            end = j;
            break;
        }
    }
    return trim(raw.substr(i, end - i));
}

// "mpr.exe" in the manifest -> the name of the program on this system.
std::string exe_name_of(const char* manifest_name) {
    std::string n = manifest_name ? manifest_name : "";
    if (n.size() > 4 && lower(n.substr(n.size() - 4)) == ".exe") n.erase(n.size() - 4);
    return n.empty() ? n : n + NXRT_EXE_SUFFIX;
}

bool read_file(const std::string& path, std::string* out) {
    std::FILE* f = fs::fopen(path, "rb");
    if (!f) return false;
    out->clear();
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    return !bad;
}

bool has_entries(const std::string& dir) {
    std::vector<fs::DirEntry> v;
    return fs::list_dir(dir, &v) && !v.empty();
}

bool setup_finished_in(const std::string& state_file) {
    std::string text;
    return read_file(state_file, &text) && nxfmt::setup_state_finished(text);
}

// The program `name` next to the launcher, started with `args`; the launcher folder is the current folder. The child is not tied to this
// process (it goes on when the launcher ends).
bool launch_in_exe_dir(const std::string& name, const std::vector<std::string>& args, proc::Child* child, std::string* err) {
    if (child) *child = proc::Child();
    const std::string dir = launcher_exe_dir();
    const std::string exe = join_path(dir, name);
    if (!fs::is_file(exe)) {
        set_err(err, name + " was not found next to the launcher.");
        return false;
    }
    if (!dir.empty() && dir != ".") fs::set_current_dir(dir);
    proc::SpawnOptions opt;
    opt.kill_with_parent = false;
    opt.hidden = false;
    proc::Child c = proc::Child::spawn(exe, args, opt);
    if (!c.valid()) {
        set_err(err, "Could not start " + name + ".");
        return false;
    }
    if (child) {
        *child = std::move(c);
    } else {
        // Child's destructor ends a running process; the game is meant to outlive the launcher, so the object is left to the end of the
        // process (no destructor runs for it).
        new proc::Child(std::move(c));
    }
    return true;
}

}  // namespace

bool GameStatus::ready() const { return exefs && romfs; }

std::string GameStatus::summary() const {
    if (ready()) {
        if (setup_finished) return "Ready to play.";
        return "Game files found, but setup did not finish. Run setup again to finish it.";
    }
    if (!exefs && !romfs && !textures && !shaders) {
        return "No game files in this folder yet. Run setup to create them from your own game dump.";
    }
    return "Some game files are missing. Run setup again.";
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    char sep = static_cast<char>(std::filesystem::path::preferred_separator);
    if (a.find('\\') != std::string::npos && a.find('/') == std::string::npos) sep = '\\';
    else if (a.find('/') != std::string::npos && a.find('\\') == std::string::npos) sep = '/';
    return a + sep + b;
}

GameStatus check_game_dir(const std::string& dir) {
    GameStatus s;
    if (dir.empty()) return s;
    s.exefs = fs::exists(join_path(join_path(dir, "exefs"), "main"));
    s.romfs = has_entries(join_path(dir, "romfs"));
    s.textures = has_entries(join_path(dir, "bc7_cache"));
    s.shaders = has_entries(join_path(dir, "shader_cache"));
    s.setup_finished = setup_finished_in(join_path(dir, nxfmt::kSetupStateName));
    return s;
}

std::string launcher_exe_dir() { return proc::exe_dir(); }

std::string default_game_dir() { return launcher_exe_dir(); }

std::string controls_path(const std::string& game_dir) { return join_path(game_dir, "controls.ini"); }

std::string product_name() { return nxrt::game::info().product ? nxrt::game::info().product : ""; }
std::string game_text(const char* utf8, const char* fallback) {
    std::string s = utf8 ? utf8 : "";
    return s.empty() && fallback ? std::string(fallback) : s;
}
std::string game_exe_name() { return exe_name_of(nxrt::game::info().exe_name); }
std::string setup_exe_name() { return exe_name_of(nxrt::game::info().setup_exe_name); }
std::string shader_pack_name() { return nxrt::game::info().shader_pack ? nxrt::game::info().shader_pack : ""; }
std::string launcher_config_path() { return join_path(launcher_exe_dir(), nxrt::game::info().launcher_ini ? nxrt::game::info().launcher_ini : ""); }

std::string format_launcher_config(const LauncherConfig& cfg) {
    std::string v = cfg.game_dir;
    // Quote the value when a plain read would cut it: spaces at the ends, or a ';' or '#' (a comment marker after a space).
    // Paths never hold a double quote, so quoting is always safe.
    bool quote = false;
    if (!v.empty()) {
        if (is_blank(v.front()) || is_blank(v.back())) quote = true;
        if (v.find(';') != std::string::npos || v.find('#') != std::string::npos) quote = true;
    }
    if (quote) v = "\"" + v + "\"";
    const nxrt::game::Info& g = nxrt::game::info();
    const char* name = (g.short_title && *g.short_title) ? g.short_title : g.product;
    return std::string("; ") + name + " launcher settings.\n[launcher]\ngame_dir = " + v + "\n";
}

bool parse_launcher_config(const std::string& utf8_text, LauncherConfig* out) {
    if (!out) return false;
    std::string text = utf8_text;
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);

    std::string section;
    bool have_dir = false;
    std::string game_dir;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string t = trim(text.substr(pos, eol - pos));
        pos = eol + 1;

        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t[0] == '[') {
            const size_t close = t.find(']');
            section = close == std::string::npos ? std::string() : lower(trim(t.substr(1, close - 1)));
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (section != "launcher" || lower(trim(t.substr(0, eq))) != "game_dir") continue;

        const std::string value = parse_value(t.substr(eq + 1));
        if (value.empty()) continue;
        if (!valid_utf8(value)) return false;
        game_dir = value;
        have_dir = true;
    }
    if (have_dir) out->game_dir = game_dir;
    return true;
}

bool load_launcher_config(const std::string& path, LauncherConfig* out) {
    if (!out) return false;
    out->game_dir = default_game_dir();
    std::string text;
    if (!read_file(path, &text)) return false;
    return parse_launcher_config(text, out);
}

bool save_launcher_config(const std::string& path, const LauncherConfig& cfg, std::string* err) {
    const std::string tmp = path + ".tmp";
    const std::string text = format_launcher_config(cfg);
    std::FILE* f = fs::fopen(tmp, "wb");
    if (!f) {
        set_err(err, "Could not write the launcher settings file.");
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        std::error_code ec;
        std::filesystem::remove(fs::native_path(tmp), ec);
        set_err(err, "Could not write the launcher settings file.");
        return false;
    }
    if (!fs::rename_replace(tmp, path, true)) {
        std::error_code ec;
        std::filesystem::remove(fs::native_path(tmp), ec);
        set_err(err, "Could not save the launcher settings.");
        return false;
    }
    return true;
}

std::vector<std::string> split_args(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool have = false, quoted = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\') {
            size_t n = 0;
            while (i < text.size() && text[i] == '\\') ++n, ++i;
            if (i < text.size() && text[i] == '"') {
                cur.append(n / 2, '\\');
                if (n % 2) {
                    cur += '"';
                } else {
                    quoted = !quoted;
                }
            } else {
                cur.append(n, '\\');
                --i;
            }
            have = true;
        } else if (c == '"') {
            quoted = !quoted;
            have = true;
        } else if ((c == ' ' || c == '\t') && !quoted) {
            if (have) out.push_back(cur);
            cur.clear();
            have = false;
        } else {
            cur += c;
            have = true;
        }
    }
    if (have) out.push_back(cur);
    return out;
}

bool start_game(const std::string& game_dir, std::string* err, const std::string& extra_args) {
    std::vector<std::string> args = {"--game", game_dir};
    for (std::string& a : split_args(extra_args)) args.push_back(std::move(a));
    return launch_in_exe_dir(game_exe_name(), args, nullptr, err);
}

bool start_setup(const std::string& game_dir, proc::Child* child, std::string* err, const std::string& extra_args) {
    std::vector<std::string> args = {"--out", game_dir};
    for (std::string& a : split_args(extra_args)) args.push_back(std::move(a));
    // The shader pack sits next to the launcher in a release. Say so explicitly, so setup finds it even when it is started from a
    // different working folder. Scripted runs that name their own shaders (or none) in --setup-args keep their choice.
    const bool has_choice = extra_args.find("--shader-pack") != std::string::npos || extra_args.find("--no-shaders") != std::string::npos;
    if (!has_choice) {
        const std::string pack = join_path(launcher_exe_dir(), shader_pack_name());
        if (!shader_pack_name().empty() && fs::exists(pack)) {
            args.push_back("--shader-pack");
            args.push_back(pack);
        }
    }
    return launch_in_exe_dir(setup_exe_name(), args, child, err);
}

// ---- Shader preparation ----------------------------------------------------------------------------------------------

PrepareLine parse_prepare_line(const std::string& raw) { return nxfmt::prepare_line_parse(raw); }

bool shader_prepare_available(const std::string& game_dir) {
    if (game_dir.empty()) return false;
    return fs::exists(join_path(join_path(game_dir, "shader_cache"), "programs.bin"));
}

bool shader_prepare_done(const std::string& game_dir) {
    if (game_dir.empty()) return false;
    std::string text;
    if (!read_file(join_path(join_path(game_dir, "shader_cache"), nxfmt::kPrepareStateName), &text)) return false;
    return nxfmt::prepare_state_decode(text).complete;
}

PrepareRun::~PrepareRun() {
    if (active_) {
        cancel();
        finish();
    }
}

bool PrepareRun::start(const std::string& game_dir, bool if_needed, std::function<void()> notify, std::string* err) {
    if (active_) {
        set_err(err, "Shader preparation is already running.");
        return false;
    }
    if (game_dir.empty()) {
        set_err(err, "No game folder is set.");
        return false;
    }
    const std::string exe = join_path(launcher_exe_dir(), game_exe_name());
    if (!fs::is_file(exe)) {
        set_err(err, game_exe_name() + " was not found next to the launcher.");
        return false;
    }
    const char* args[4] = {exe.c_str(), "--prepare-shaders", if_needed ? "--if-needed" : nullptr, nullptr};
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(p, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_SetStringProperty(p, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, game_dir.c_str());
    proc_ = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    if (!proc_) {
        set_err(err, std::string("Could not start shader preparation: ") + SDL_GetError());
        return false;
    }
    notify_ = std::move(notify);
    {
        std::lock_guard<std::mutex> lk(mu_);
        progress_ = PrepareLine{};
        result_ = PrepareLine{};
    }
    done_ = false;
    cancel_requested_ = false;
    exit_code_ = -1;
    active_ = true;
    thread_ = std::thread(&PrepareRun::worker, this);
    return true;
}

void PrepareRun::cancel() {
    if (!active_ || !proc_) return;
    cancel_requested_ = true;
    SDL_KillProcess(proc_, true);   // only this run's child; the next run starts from zero
}

PrepareLine PrepareRun::progress() const {
    std::lock_guard<std::mutex> lk(mu_);
    return progress_;
}

PrepareLine PrepareRun::result() const {
    std::lock_guard<std::mutex> lk(mu_);
    return result_;
}

int PrepareRun::finish() {
    if (thread_.joinable()) thread_.join();
    if (proc_) {
        SDL_DestroyProcess(proc_);
        proc_ = nullptr;
    }
    active_ = false;
    return exit_code_;
}

void PrepareRun::handle_line(const std::string& line) {
    const PrepareLine pl = parse_prepare_line(line);
    if (pl.kind == PrepareKind::other) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pl.kind == PrepareKind::progress) {
            progress_ = pl;
        } else {
            result_ = pl;
        }
    }
    if (notify_) notify_();
}

void PrepareRun::worker() {
    std::string acc;
    char buf[4096];
    if (SDL_IOStream* out = SDL_GetProcessOutput(proc_)) {
        for (;;) {
            const size_t n = SDL_ReadIO(out, buf, sizeof buf);
            if (n == 0) break;   // the child closed its output
            acc.append(buf, n);
            size_t p;
            while ((p = acc.find('\n')) != std::string::npos) {
                handle_line(acc.substr(0, p));
                acc.erase(0, p + 1);
            }
        }
    }
    if (!acc.empty()) handle_line(acc);
    int code = -1;
    if (!SDL_WaitProcess(proc_, true, &code)) code = -1;
    exit_code_ = code;
    done_ = true;
    if (notify_) notify_();
}

}  // namespace nxl
