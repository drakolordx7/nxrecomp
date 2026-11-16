// SPDX-License-Identifier: GPL-3.0-or-later
#include "keys.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <system_error>
#include <vector>

#include "crypto.h"
#include "fileio.h"
#include "log.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#endif

namespace nxs {
namespace {

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && is_blank(s[b])) ++b;
    while (e > b && is_blank(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool all_hex(std::string_view s) {
    for (char c : s) {
        if (!is_hex_digit(c)) return false;
    }
    return true;
}

std::string two_hex(int g) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02x", static_cast<unsigned>(g));
    return buf;
}

void ecb_decrypt(const u8 key[16], const void* in, void* out, size_t n) {
    Aes128Ecb(key).decrypt(in, out, n);
}

bool file_exists(const std::string& utf8_path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(fs_path(utf8_path), ec);
}

std::string full_path(const std::string& utf8) {
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(fs_path(utf8), ec);
    return ec ? utf8 : path_utf8(abs.lexically_normal());
}

std::string env_var(const char* name) {
#ifdef _WIN32
    const wchar_t* v = _wgetenv(widen(name).c_str());   // the wide environment: a user name outside the ANSI code page survives
    return v ? narrow(v) : std::string();
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

// The per-user config folder the emulators keep their keys under: %APPDATA% (Roaming) on Windows, $XDG_CONFIG_HOME or ~/.config elsewhere.
std::string config_dir() {
#ifdef _WIN32
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) out = narrow(p);
    if (p) CoTaskMemFree(p);
    return out;
#else
    const std::string xdg = env_var("XDG_CONFIG_HOME");
    if (!xdg.empty()) return xdg;
    const std::string home = env_var("HOME");
    return home.empty() ? std::string() : home + "/.config";
#endif
}

// The per-user data folder (yuzu and its forks keep keys under .local/share on Linux): $XDG_DATA_HOME or ~/.local/share. Empty on Windows.
std::string data_dir() {
#ifdef _WIN32
    return {};
#else
    const std::string xdg = env_var("XDG_DATA_HOME");
    if (!xdg.empty()) return xdg;
    const std::string home = env_var("HOME");
    return home.empty() ? std::string() : home + "/.local/share";
#endif
}

}  // namespace

void KeySet::load_file(const std::string& utf8_path) {
    std::string data;
    if (!read_text_file(utf8_path, data)) {
        throw Error(Fail::Keys, "Cannot read the keys file " + utf8_path);
    }

    size_t pos = data.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    int skipped = 0;
    while (pos < data.size()) {
        const size_t nl = data.find('\n', pos);
        const size_t end = nl == std::string::npos ? data.size() : nl;
        const std::string line = trim(std::string_view(data).substr(pos, end - pos));
        pos = nl == std::string::npos ? data.size() : nl + 1;

        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            ++skipped;
            continue;
        }
        const std::string name = lower(trim(std::string_view(line).substr(0, eq)));
        const std::string value = trim(std::string_view(line).substr(eq + 1));
        if (name.empty() || value.empty() || value.size() > 256 || value.size() % 2 != 0 || !all_hex(value)) {
            ++skipped;
            continue;
        }

        if (name.size() == 32 && value.size() == 32 && all_hex(name)) {   // title key: rights id = key
            Key128 key{};
            if (!hex_decode(value, key.data(), key.size())) {
                ++skipped;
                continue;
            }
            title_keys_[name] = key;
            continue;
        }
        std::vector<u8> bytes(value.size() / 2);
        if (!hex_decode(value, bytes.data(), bytes.size())) {
            ++skipped;
            continue;
        }
        keys_[name] = std::move(bytes);
    }

    if (skipped != 0) NXS_LOGD("Skipped %d malformed line(s) in a keys file.", skipped);
    if (keys_.empty() && title_keys_.empty()) {
        throw Error(Fail::Keys, "The file " + utf8_path + " does not look like a keys file.");
    }
    NXS_LOGI("Keys file loaded: %zu keys, %zu title keys.", keys_.size(), title_keys_.size());
}

bool KeySet::get(const std::string& name, u8* out, size_t n) const {
    const auto it = keys_.find(name);
    if (it == keys_.end() || it->second.size() != n) return false;
    std::memcpy(out, it->second.data(), n);
    return true;
}

Key128 KeySet::master_key(int generation) const {
    const std::string name = "master_key_" + two_hex(generation);
    Key128 key{};
    if (!get(name, key.data(), key.size())) {
        throw Error(Fail::Keys, "Your keys file has no " + name + " (needed for games of key generation " +
                                    std::to_string(generation + 1) + "). Dump newer keys with Lockpick_RCM.");
    }
    return key;
}

Key128 KeySet::titlekek(int generation) const {
    const auto need = [this](const std::string& name, u8* out, size_t n) {
        if (!get(name, out, n)) {
            throw Error(Fail::Keys, "Your keys file has no " + name + ", which is needed to work out a title key. "
                                    "Dump newer keys with Lockpick_RCM.");
        }
    };
    Key128 out{};
    if (get("titlekek_" + two_hex(generation), out.data(), out.size())) return out;

    const Key128 mk = master_key(generation);
    Key128 src{};
    need("titlekek_source", src.data(), src.size());
    ecb_decrypt(mk.data(), src.data(), out.data(), 16);
    return out;
}

Key128 KeySet::key_area_key(int kind, int generation) const {
    static const char* const kinds[] = {"application", "ocean", "system"};
    if (kind < 0 || kind > 2) throw Error(Fail::Generic, "Unknown key area key kind.");
    const std::string kind_name = kinds[kind];

    Key128 out{};
    if (get("key_area_key_" + kind_name + "_" + two_hex(generation), out.data(), out.size())) return out;

    const auto need = [this](const std::string& name, u8* dst, size_t n) {
        if (!get(name, dst, n)) {
            throw Error(Fail::Keys, "Your keys file has no " + name + ", which is needed to work out a key area key. "
                                    "Dump newer keys with Lockpick_RCM.");
        }
    };
    const Key128 mk = master_key(generation);
    Key128 kek_src{}, src{}, key_gen_src{};
    need("aes_kek_generation_source", kek_src.data(), kek_src.size());
    need("key_area_key_" + kind_name + "_source", src.data(), src.size());
    need("aes_key_generation_source", key_gen_src.data(), key_gen_src.size());

    Key128 kek{}, src_kek{};
    ecb_decrypt(mk.data(), kek_src.data(), kek.data(), 16);
    ecb_decrypt(kek.data(), src.data(), src_kek.data(), 16);
    ecb_decrypt(src_kek.data(), key_gen_src.data(), out.data(), 16);
    return out;
}

std::array<u8, 32> KeySet::header_key() const {
    const auto need = [this](const std::string& name, u8* dst, size_t n) {
        if (!get(name, dst, n)) {
            throw Error(Fail::Keys, "Your keys file has no " + name + ", which is needed to work out the header key. "
                                    "Dump newer keys with Lockpick_RCM.");
        }
    };
    std::array<u8, 32> out{};
    if (get("header_key", out.data(), out.size())) return out;

    const Key128 mk0 = master_key(0);
    Key128 aes_kek_src{}, header_kek_src{}, key_gen_src{};
    std::array<u8, 32> header_src{};
    need("aes_kek_generation_source", aes_kek_src.data(), aes_kek_src.size());
    need("header_kek_source", header_kek_src.data(), header_kek_src.size());
    need("aes_key_generation_source", key_gen_src.data(), key_gen_src.size());
    need("header_key_source", header_src.data(), header_src.size());

    Key128 kek{}, header_kek{}, header_key_key{};
    ecb_decrypt(mk0.data(), aes_kek_src.data(), kek.data(), 16);
    ecb_decrypt(kek.data(), header_kek_src.data(), header_kek.data(), 16);
    ecb_decrypt(header_kek.data(), key_gen_src.data(), header_key_key.data(), 16);
    ecb_decrypt(header_key_key.data(), header_src.data(), out.data(), out.size());
    return out;
}

bool KeySet::title_key(const std::array<u8, 16>& rights_id, Key128& out) const {
    const auto it = title_keys_.find(hex_encode(rights_id.data(), rights_id.size()));
    if (it == title_keys_.end()) return false;
    out = it->second;
    return true;
}

std::string KeySet::describe() const {
    const std::string prefix = "master_key_";
    int lo = -1, hi = -1;
    for (const auto& kv : keys_) {
        const std::string& name = kv.first;
        if (name.size() != prefix.size() + 2 || name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string suffix = name.substr(prefix.size());
        if (!all_hex(suffix)) continue;
        const int g = std::stoi(suffix, nullptr, 16);
        if (lo < 0 || g < lo) lo = g;
        if (hi < 0 || g > hi) hi = g;
    }
    char buf[160];
    if (lo < 0) {
        std::snprintf(buf, sizeof buf, "%zu keys, no master keys, %zu title keys", keys_.size(), title_keys_.size());
    } else {
        std::snprintf(buf, sizeof buf, "%zu keys, master keys %02x..%02x, %zu title keys", keys_.size(),
                      static_cast<unsigned>(lo), static_cast<unsigned>(hi), title_keys_.size());
    }
    return buf;
}

std::vector<KeyFileCandidate> find_key_files() {
    std::vector<KeyFileCandidate> found;
    std::set<std::string> seen;
    const auto consider = [&](const std::string& path, const char* source) {
        if (path.empty()) return;
        const std::string full = full_path(path);
        if (!file_exists(full)) return;
#ifdef _WIN32
        if (!seen.insert(lower(full)).second) return;   // Windows paths do not differ by case
#else
        if (!seen.insert(full).second) return;
#endif
        found.push_back({full, source});
    };

    const std::string cfg = config_dir();
    if (!cfg.empty()) {
        consider(path_join(cfg, "yuzu/keys/prod.keys"), "yuzu");
        consider(path_join(cfg, "eden/keys/prod.keys"), "Eden");
        consider(path_join(cfg, "suyu/keys/prod.keys"), "suyu");
        consider(path_join(cfg, "sudachi/keys/prod.keys"), "Sudachi");
        consider(path_join(cfg, "citron/keys/prod.keys"), "Citron");
        consider(path_join(cfg, "Ryujinx/system/prod.keys"), "Ryujinx");
    }
    const std::string data = data_dir();
    if (!data.empty()) {   // Linux: yuzu, Eden and their forks keep the keys under ~/.local/share
        consider(path_join(data, "yuzu/keys/prod.keys"), "yuzu");
        consider(path_join(data, "eden/keys/prod.keys"), "Eden");
        consider(path_join(data, "suyu/keys/prod.keys"), "suyu");
        consider(path_join(data, "sudachi/keys/prod.keys"), "Sudachi");
        consider(path_join(data, "citron/keys/prod.keys"), "Citron");
    }
#ifdef _WIN32
    const std::string profile = env_var("USERPROFILE");
#else
    const std::string profile = env_var("HOME");
#endif
    if (!profile.empty()) consider(path_join(profile, ".switch/prod.keys"), "~/.switch");
    const std::string exe = exe_dir();
    if (!exe.empty()) {
        consider(path_join(exe, "prod.keys"), "next to the program");
        consider(path_join(path_join(exe, "keys"), "prod.keys"), "next to the program");
    }
    consider("prod.keys", "current folder");
    return found;
}

std::string sibling_title_keys(const std::string& prod_keys_path) {
    const size_t slash = prod_keys_path.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? std::string() : prod_keys_path.substr(0, slash + 1);
    const std::string candidate = dir + "title.keys";
    return file_exists(candidate) ? candidate : std::string();
}

}  // namespace nxs
