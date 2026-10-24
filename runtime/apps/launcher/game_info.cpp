// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_info.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace mprl {

namespace fs = std::filesystem;

namespace {

void set_err(std::wstring* err, const std::wstring& msg) {
    if (err) *err = msg;
}

bool is_blank(char c) { return c == ' ' || c == '\t'; }

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (is_blank(s[b]) || s[b] == '\r')) ++b;
    while (e > b && (is_blank(s[e - 1]) || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// ASCII lower case, for section and key names.
std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return s;
}

bool utf8_to_wide(const std::string& in, std::wstring* out) {
    out->clear();
    if (in.empty()) return true;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.data(), static_cast<int>(in.size()), nullptr, 0);
    if (n <= 0) return false;
    out->resize(static_cast<size_t>(n));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.data(), static_cast<int>(in.size()), out->data(), n) != n) {
        out->clear();
        return false;
    }
    return true;
}

std::string wide_to_utf8(const std::wstring& in) {
    if (in.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, in.data(), static_cast<int>(in.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, in.data(), static_cast<int>(in.size()), out.data(), n, nullptr, nullptr);
    return out;
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

// Starts <exe dir>\exe_name with the given argument text. Waits for nothing. The handle goes to *process if given,
// otherwise it is closed here.
bool launch_in_exe_dir(const std::wstring& exe_name, const std::wstring& args, HANDLE* process, std::wstring* err) {
    if (process) *process = nullptr;
    const std::wstring dir = launcher_exe_dir();
    const std::wstring exe = (fs::path(dir) / exe_name).wstring();
    std::error_code ec;
    if (!fs::exists(exe, ec)) {
        set_err(err, exe_name + L" was not found next to the launcher.");
        return false;
    }
    std::wstring cmd = quote_arg(exe) + L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                   dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    if (!ok) {
        const DWORD code = GetLastError();
        set_err(err, L"Could not start " + exe_name + L" (Windows error " + std::to_wstring(code) + L").");
        return false;
    }
    CloseHandle(pi.hThread);
    if (process) {
        *process = pi.hProcess;
    } else {
        CloseHandle(pi.hProcess);
    }
    return true;
}

bool path_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// True when the folder exists and has at least one entry.
bool has_entries(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    fs::directory_iterator it(dir, ec);
    return !ec && it != fs::directory_iterator();
}

// setup_state.tsv as the setup tool writes it (pipeline.cpp, State): first line "mprs_setup_state\t1", then
// "meta\tkey\tvalue" lines; the last "meta\tfinished" line wins. Anything else in the file is ignored.
bool setup_finished_in(const fs::path& state_file) {
    std::ifstream f(state_file, std::ios::binary);
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != "mprs_setup_state\t1") return false;

    bool finished = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> fields;
        size_t p = 0;
        for (;;) {
            const size_t q = line.find('\t', p);
            fields.push_back(line.substr(p, q == std::string::npos ? q : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (fields.size() >= 3 && fields[0] == "meta" && fields[1] == "finished") finished = (fields[2] == "1");
    }
    return finished;
}

}  // namespace

bool GameStatus::ready() const { return exefs && romfs; }

std::wstring GameStatus::summary() const {
    if (ready()) {
        if (setup_finished) return L"Ready to play.";
        return L"Game files found, but setup did not finish. Run setup again to finish it.";
    }
    if (!exefs && !romfs && !textures && !shaders) {
        return L"No game files in this folder yet. Run setup to create them from your own game dump.";
    }
    return L"Some game files are missing. Run setup again.";
}

GameStatus check_game_dir(const std::wstring& dir) {
    GameStatus s;
    if (dir.empty()) return s;
    const fs::path root(dir);
    s.exefs = path_exists(root / L"exefs" / L"main");
    s.romfs = has_entries(root / L"romfs");
    s.textures = has_entries(root / L"bc7_cache");
    s.shaders = has_entries(root / L"shader_cache");
    s.setup_finished = setup_finished_in(root / L"setup_state.tsv");
    return s;
}

std::wstring launcher_exe_dir() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) break;   // fits (n is the length without the terminator)
        if (buf.size() >= 65536) return {};
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf.data()).parent_path().wstring();
}

std::wstring default_game_dir() { return launcher_exe_dir(); }

std::wstring controls_path(const std::wstring& game_dir) { return (fs::path(game_dir) / L"controls.ini").wstring(); }

std::wstring launcher_config_path() { return (fs::path(launcher_exe_dir()) / L"mpr_launcher.ini").wstring(); }

std::string format_launcher_config(const LauncherConfig& cfg) {
    std::string v = wide_to_utf8(cfg.game_dir);
    // Quote the value when a plain read would cut it: spaces at the ends, or a ';' or '#' (a comment marker after a space).
    // Windows paths never hold a double quote, so quoting is always safe.
    bool quote = false;
    if (!v.empty()) {
        if (is_blank(v.front()) || is_blank(v.back())) quote = true;
        if (v.find(';') != std::string::npos || v.find('#') != std::string::npos) quote = true;
    }
    if (quote) v = "\"" + v + "\"";
    return "; Metroid Prime launcher settings.\n[launcher]\ngame_dir = " + v + "\n";
}

bool parse_launcher_config(const std::string& utf8_text, LauncherConfig* out) {
    if (!out) return false;
    std::string text = utf8_text;
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);

    std::string section;
    bool have_dir = false;
    std::wstring game_dir;
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
        if (value.empty()) continue;   // empty value keeps the default
        std::wstring w;
        if (!utf8_to_wide(value, &w)) return false;
        game_dir = w;
        have_dir = true;
    }
    if (have_dir) out->game_dir = game_dir;
    return true;
}

bool load_launcher_config(const std::wstring& path, LauncherConfig* out) {
    if (!out) return false;
    out->game_dir = default_game_dir();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (f.bad()) return false;
    return parse_launcher_config(text, out);
}

bool save_launcher_config(const std::wstring& path, const LauncherConfig& cfg, std::wstring* err) {
    const std::wstring tmp = path + L".tmp";
    const std::string text = format_launcher_config(cfg);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            set_err(err, L"Could not write the launcher settings file.");
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.close();
        if (f.fail()) {
            std::error_code ec;
            fs::remove(tmp, ec);
            set_err(err, L"Could not write the launcher settings file.");
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        std::error_code ec;
        fs::remove(tmp, ec);
        set_err(err, L"Could not save the launcher settings (Windows error " + std::to_wstring(code) + L").");
        return false;
    }
    return true;
}

std::wstring quote_arg(const std::wstring& s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    size_t i = 0;
    for (;;) {
        size_t backslashes = 0;
        while (i < s.size() && s[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == s.size()) {
            out.append(backslashes * 2, L'\\');   // backslashes before the closing quote are doubled
            break;
        }
        if (s[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(s[i]);
        }
        ++i;
    }
    out.push_back(L'"');
    return out;
}

bool start_game(const std::wstring& game_dir, std::wstring* err, const std::wstring& extra_args) {
    std::wstring args = L"--game " + quote_arg(game_dir);
    if (!extra_args.empty()) args += L" " + extra_args;
    return launch_in_exe_dir(L"mpr.exe", args, nullptr, err);
}

bool start_setup(const std::wstring& game_dir, HANDLE* process, std::wstring* err, const std::wstring& extra_args) {
    std::wstring args = L"--out " + quote_arg(game_dir);
    if (!extra_args.empty()) args += L" " + extra_args;
    return launch_in_exe_dir(L"mpr_setup.exe", args, process, err);
}

}  // namespace mprl
