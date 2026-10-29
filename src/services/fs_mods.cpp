// SPDX-License-Identifier: GPL-3.0-or-later
// Mod discovery, mods.ini and the override index. See fs_mods.h for the layouts, the priority rules and the sources.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fs_mods.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/services.h"

namespace stdfs = std::filesystem;

namespace nxrt::services::fs {
namespace {

constexpr int kMaxSearchDepth = 8;       // folder levels searched below the mods folder
constexpr int kMaxScanDepth = 64;        // folder levels inside a romfs folder (guards against link loops)
constexpr size_t kHostLongPath = 240;    // host paths this long or longer get the \\?\ prefix
constexpr size_t kCaseWarnPerMod = 5;    // case warnings logged per mod before they are only counted

// ---- strings and paths -------------------------------------------------------------------------------------------------------------

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}
bool ieq(const std::string& a, const std::string& b) { return a.size() == b.size() && lower(a) == lower(b); }
bool ends_with_i(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && lower(s.substr(s.size() - n)) == suffix;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

bool to_utf8(const std::wstring& w, std::string& out) {
    out.clear();
    if (w.empty()) return true;
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr) == n;
}

stdfs::path u8(const std::string& s) { return stdfs::u8path(s); }
std::string path_u8(const stdfs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// A host path the Win32 file calls accept whatever its length: absolute, backslashes, \\?\ prefix when long.
std::wstring ready_path(const stdfs::path& p) {
    std::error_code ec;
    stdfs::path a = stdfs::absolute(p, ec);
    std::wstring w = (ec ? p : a).lexically_normal().wstring();
    std::replace(w.begin(), w.end(), L'/', L'\\');
    if (w.size() < kHostLongPath || w.compare(0, 4, L"\\\\?\\") == 0) return w;
    if (w.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    return L"\\\\?\\" + w;
}

stdfs::path exe_dir() {
    wchar_t buf[4096];
    const DWORD n = GetModuleFileNameW(nullptr, buf, 4096);
    if (n == 0 || n >= 4096) return stdfs::path(".");
    return stdfs::path(std::wstring(buf, n)).parent_path();
}

bool is_title_id(const std::string& s) {
    if (s.size() != 16) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

// "A\B//c " -> "A/B/c": the form a mod name takes in mods.ini and the log.
std::string norm_name(const std::string& s) {
    std::string out, cur;
    auto flush = [&] {
        cur = trim(cur);
        if (!cur.empty()) {
            if (!out.empty()) out += '/';
            out += cur;
        }
        cur.clear();
    };
    for (char c : s) {
        if (c == '/' || c == '\\') flush();
        else cur += c;
    }
    flush();
    return out;
}

std::string join_path(const std::string& dir, const std::string& name) { return dir == "/" ? "/" + name : dir + "/" + name; }
std::string parent_path(const std::string& p) {
    const size_t i = p.rfind('/');
    return i == 0 || i == std::string::npos ? std::string("/") : p.substr(0, i);
}
std::string leaf_name(const std::string& p) {
    const size_t i = p.rfind('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

// ---- directory listing -------------------------------------------------------------------------------------------------------------

struct Ent {
    std::string name;
    stdfs::path path;
    bool dir = false, reg = false;
    uint64_t size = 0;
};

// Entries of a host folder (links are followed). Names that are not valid UTF-8 are counted in *bad and left out.
bool list_entries(const stdfs::path& dir, std::vector<Ent>& out, size_t* bad = nullptr) {
    out.clear();
    std::error_code ec;
    stdfs::directory_iterator it(dir, stdfs::directory_options::skip_permission_denied, ec);
    const stdfs::directory_iterator end;
    if (ec) return false;
    for (; !ec && it != end; it.increment(ec)) {
        const stdfs::directory_entry& e = *it;
        Ent x;
        if (!to_utf8(e.path().filename().wstring(), x.name) || x.name.empty()) {
            if (bad) ++*bad;
            continue;
        }
        std::error_code se;
        const stdfs::file_status st = e.status(se);
        if (se) continue;
        x.path = e.path();
        x.dir = stdfs::is_directory(st);
        x.reg = stdfs::is_regular_file(st);
        if (x.reg) {
            std::error_code fe;
            x.size = e.file_size(fe);
            if (fe) continue;
        }
        out.push_back(std::move(x));
    }
    std::sort(out.begin(), out.end(), [](const Ent& a, const Ent& b) { return a.name < b.name; });
    return true;
}

// ---- discovery ---------------------------------------------------------------------------------------------------------------------

struct FoundMod {
    std::string name;                        // path below the mods folder without container folders ("MyMod", "Pack/Sub")
    std::vector<stdfs::path> romfs_dirs, ext_dirs, bins;
    std::vector<std::string> unsupported;    // what the mod holds that cannot be used
    std::string folder;                      // the folder that held the markers
};

struct Discovery {
    std::vector<FoundMod> mods;
    std::vector<std::string> global_unsupported;   // mods/exefs_patches and the like
    size_t skipped_other_title = 0;
};

void add_unsupported(FoundMod& m, const std::string& what) {
    if (std::find(m.unsupported.begin(), m.unsupported.end(), what) == m.unsupported.end()) m.unsupported.push_back(what);
}

std::string make_name(const std::vector<std::string>& rel, const std::string& first) {
    std::string out;
    std::string prev;
    for (const std::string& s : rel) {
        if (!prev.empty() && ieq(prev, s)) continue;   // "Cool Mod/Cool Mod": a zip's wrapper folder repeating the mod's name
        prev = s;
        if (!out.empty()) out += '/';
        out += s;
    }
    if (out.empty()) out = first.empty() ? std::string("(mods folder)") : first;
    return out;
}

void visit(Discovery& d, uint64_t title, const stdfs::path& dir, const std::vector<std::string>& rel, const std::string& first, int depth) {
    if (depth > kMaxSearchDepth) {
        NXLOG_D("mods", "not searching deeper than %d folders: '%s'", kMaxSearchDepth, path_u8(dir).c_str());
        return;
    }
    std::vector<Ent> ents;
    if (!list_entries(dir, ents)) return;

    FoundMod cur;
    bool is_mod = false;
    std::set<std::string> marker_dirs;
    for (const Ent& e : ents) {
        const std::string l = lower(e.name);
        if (e.dir) {
            if (l == "romfs") {
                cur.romfs_dirs.push_back(e.path);
                is_mod = true;
            } else if (l == "romfs_ext") {
                cur.ext_dirs.push_back(e.path);
                is_mod = true;
            } else if (l == "exefs" || l == "exefs_patches" || l == "nro_patches" || l == "kip_patches" || l == "cheats") {
                add_unsupported(cur, l);
                is_mod = true;
            } else {
                continue;
            }
            marker_dirs.insert(e.name);
        } else if (e.reg) {
            if (l == "romfs.bin") {
                cur.bins.push_back(e.path);
                is_mod = true;
            } else if (l == "exefs.nsp") {
                add_unsupported(cur, "exefs.nsp");
                is_mod = true;
            }
        }
    }
    if (is_mod) {
        cur.name = make_name(rel, first);
        cur.folder = path_u8(dir);
        FoundMod* same = nullptr;
        for (FoundMod& m : d.mods)
            if (ieq(m.name, cur.name)) same = &m;
        if (!same) {
            d.mods.push_back(std::move(cur));
        } else {   // a second root of the same mod (e.g. romfs/ and atmosphere/contents/<id>/romfs/)
            for (auto& p : cur.romfs_dirs) same->romfs_dirs.push_back(p);
            for (auto& p : cur.ext_dirs) same->ext_dirs.push_back(p);
            for (auto& p : cur.bins) same->bins.push_back(p);
            for (auto& u : cur.unsupported) add_unsupported(*same, u);
        }
    }
    for (const Ent& e : ents) {
        if (!e.dir || marker_dirs.count(e.name)) continue;
        const std::string l = lower(e.name);
        if (is_title_id(e.name)) {
            const uint64_t id = std::strtoull(e.name.c_str(), nullptr, 16);
            if (title != 0 && id != title) {
                d.skipped_other_title++;
                NXLOG_I("mods", "skipping '%s': it is for title %s, this game is %016llX", path_u8(e.path).c_str(), e.name.c_str(),
                        static_cast<unsigned long long>(title));
                continue;
            }
            visit(d, title, e.path, rel, first.empty() ? e.name : first, depth + 1);
        } else if (l == "contents" || l == "atmosphere") {
            visit(d, title, e.path, rel, first.empty() ? e.name : first, depth + 1);
        } else {
            std::vector<std::string> r = rel;
            r.push_back(e.name);
            visit(d, title, e.path, r, first.empty() ? e.name : first, depth + 1);
        }
    }
}

Discovery discover(const stdfs::path& root, uint64_t title) {
    Discovery d;
    std::vector<Ent> top;
    if (!list_entries(root, top)) return d;
    for (const Ent& e : top) {
        const std::string l = lower(e.name);
        if (!e.dir) {
            if (ends_with_i(e.name, ".zip") || ends_with_i(e.name, ".7z") || ends_with_i(e.name, ".rar") || ends_with_i(e.name, ".tar") ||
                ends_with_i(e.name, ".gz") || ends_with_i(e.name, ".xz"))
                NXLOG_W("mods", "'%s' is an archive: extract it into its own folder inside the mods folder (mods/<name>/romfs/...); it is ignored as it is",
                        e.name.c_str());
            continue;
        }
        if (l == "exefs_patches" || l == "nro_patches" || l == "kip_patches") {
            d.global_unsupported.push_back(e.name);
            continue;
        }
        if (l == "romfs" || l == "romfs_ext") {
            NXLOG_W("mods", "'%s' sits directly in the mods folder: put each mod in its own folder (mods/<mod name>/%s/...); it is ignored",
                    e.name.c_str(), l.c_str());
            continue;
        }
        const size_t before = d.mods.size();
        const size_t skipped_before = d.skipped_other_title;
        const bool container = l == "contents" || l == "atmosphere" || is_title_id(e.name);
        if (is_title_id(e.name)) {
            const uint64_t id = std::strtoull(e.name.c_str(), nullptr, 16);
            if (title != 0 && id != title) {
                d.skipped_other_title++;
                NXLOG_I("mods", "skipping '%s': it is for title %s, this game is %016llX", e.name.c_str(), e.name.c_str(),
                        static_cast<unsigned long long>(title));
                continue;
            }
        }
        std::vector<std::string> rel;
        if (!container) rel.push_back(e.name);
        visit(d, title, e.path, rel, e.name, 1);
        if (d.mods.size() == before && d.skipped_other_title == skipped_before)
            NXLOG_W("mods", "folder '%s' holds no RomFS content and is ignored. A mod needs a romfs folder: %s/romfs/<files as in the game's RomFS>, or "
                            "atmosphere/contents/<title id>/romfs/... inside it, or a romfs.bin",
                    e.name.c_str(), e.name.c_str());
    }
    std::sort(d.mods.begin(), d.mods.end(), [](const FoundMod& a, const FoundMod& b) { return lower(a.name) < lower(b.name); });
    return d;
}

// ---- mods.ini ----------------------------------------------------------------------------------------------------------------------
// ; comments
// [order]                  (the section may also be called [mods])
// Mod name = on            highest priority first; "on"/"off" (also true/false, yes/no, 1/0)

struct ModsIni {
    struct Entry {
        std::string name;    // normalised
        bool on = true;
    };
    std::vector<std::string> lines;
    std::vector<Entry> entries;
    long section_line = -1;      // the [order] line
    long last_entry_line = -1;   // the last entry line of that section
    bool existed = false;
};

bool parse_flag(const std::string& v, bool* ok) {
    const std::string l = lower(trim(v));
    *ok = true;
    if (l == "on" || l == "true" || l == "yes" || l == "1" || l == "enabled" || l.empty()) return true;
    if (l == "off" || l == "false" || l == "no" || l == "0" || l == "disabled") return false;
    *ok = false;
    return true;
}

ModsIni read_ini(const std::string& path) {
    ModsIni ini;
    if (path.empty()) return ini;
    std::ifstream f(u8(path), std::ios::binary);
    if (!f) return ini;
    ini.existed = true;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    std::string line;
    std::istringstream in(text);
    bool in_order = false;
    std::set<std::string> seen;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        ini.lines.push_back(line);
        const long idx = static_cast<long>(ini.lines.size()) - 1;
        const std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t[0] == '[') {
            const size_t close = t.find(']');
            const std::string sec = lower(trim(t.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            in_order = sec == "order" || sec == "mods";
            if (in_order && ini.section_line < 0) {
                ini.section_line = idx;
                ini.last_entry_line = idx;
            }
            continue;
        }
        if (!in_order) continue;
        ModsIni::Entry e;
        const size_t eq = t.rfind('=');
        std::string val;
        if (eq == std::string::npos) {
            e.name = norm_name(t);
        } else {
            e.name = norm_name(t.substr(0, eq));
            val = t.substr(eq + 1);
        }
        if (e.name.empty()) continue;
        bool ok = true;
        e.on = parse_flag(val, &ok);
        if (!ok) NXLOG_W("mods", "mods.ini line %ld: '%s' is not on/off for '%s'; the mod is treated as on", idx + 1, trim(val).c_str(), e.name.c_str());
        if (!seen.insert(lower(e.name)).second) {
            NXLOG_W("mods", "mods.ini line %ld: '%s' is listed twice; the first entry counts", idx + 1, e.name.c_str());
            continue;
        }
        ini.entries.push_back(std::move(e));
        ini.last_entry_line = idx;
    }
    return ini;
}

// Appends `added` (enabled) to the order section, keeping every other line as the player wrote it.
bool write_ini(const std::string& path, ModsIni& ini, const std::vector<std::string>& added) {
    if (path.empty() || added.empty()) return true;
    std::vector<std::string> out = ini.lines;
    std::vector<std::string> add;
    for (const std::string& n : added) add.push_back(n + " = on");
    if (ini.section_line < 0) {
        if (!out.empty() && !trim(out.back()).empty()) out.push_back("");
        if (!ini.existed) {
            out.push_back("; Mods used by the game, one per line: name = on or off.");
            out.push_back("; A mod listed higher wins over a mod listed below it, and every mod wins over the game's own files.");
            out.push_back("; Mods found in the mods folder are added at the bottom (lowest priority), switched on.");
            out.push_back("; Names match the mod folders (a mod inside a wrapper folder is written Wrapper/Mod).");
        }
        out.push_back("[order]");
        for (auto& a : add) out.push_back(a);
    } else {
        out.insert(out.begin() + ini.last_entry_line + 1, add.begin(), add.end());
    }
    std::string text;
    for (const std::string& l : out) text += l + "\r\n";
    const stdfs::path p = u8(path);
    const stdfs::path tmp = stdfs::path(p.wstring() + L".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) return false;
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        stdfs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace

// ---- ModOverlay lookups ------------------------------------------------------------------------------------------------------------

const ModSource* ModOverlay::find_file(const std::string& path) const {
    auto it = files_.find(path);
    return it == files_.end() ? nullptr : &it->second.src;
}

const std::wstring* ModOverlay::dir_host(const std::string& path) const {
    auto it = dirs_.find(path);
    return it == dirs_.end() ? nullptr : &it->second.host;
}

const std::vector<ModOverlay::Child>* ModOverlay::children(const std::string& dir) const {
    auto it = dirs_.find(dir);
    return it == dirs_.end() ? nullptr : &it->second.kids;
}

bool ModOverlay::hidden(const std::string& path) const {
    if (hidden_.empty()) return false;
    std::string p = path;
    for (;;) {
        if (hidden_.count(p)) return true;
        if (p.size() <= 1) return false;
        p = parent_path(p);
    }
}

// ---- building the index ------------------------------------------------------------------------------------------------------------

class OverlayBuilder {
public:
    explicit OverlayBuilder(ModOverlay& o) : o_(o) { o_.dirs_["/"]; }

    void begin_mod(uint32_t idx) { cur_ = idx; }

    void ensure_dir(const std::string& path, const std::wstring& host) {
        if (o_.dirs_.count(path)) return;
        if (path != "/") {
            const std::string par = parent_path(path);
            ensure_dir(par, host);
            if (o_.files_.count(path)) {   // a folder over a file of a lower mod: the higher mod wins
                NXLOG_W("mods", "'%s' is a file in a lower-priority mod and a folder in '%s'; the file is dropped", path.c_str(), name_of(cur_).c_str());
                drop_file(path);
                remove_kid(par, leaf_name(path));
            }
            o_.dirs_[par].kids.push_back({leaf_name(path), true});
        }
        o_.dirs_[path].host = host;
    }

    // `dir` must exist (ensure_dir). Returns false when the file was not added.
    void add_file(const std::string& dir, const std::string& name, ModSource src) {
        const std::string path = join_path(dir, name);
        if (o_.dirs_.count(path)) {   // a file over a folder of a lower mod
            NXLOG_W("mods", "'%s' is a folder in a lower-priority mod and a file in '%s'; the folder is dropped", path.c_str(), name_of(cur_).c_str());
            remove_subtree(path);
            remove_kid(dir, name);
        }
        auto it = o_.files_.find(path);
        if (it != o_.files_.end()) {
            if (it->second.mod == cur_) {   // the loose file over the same mod's romfs.bin member
                it->second.src = std::move(src);
                return;
            }
            const uint32_t prev = it->second.mod;
            o_.infos_[prev].lost++;
            Conflict& c = conflicts_[(static_cast<uint64_t>(cur_) << 32) | prev];
            if (c.count++ == 0) c.example = path;
            NXLOG_D("mods", "conflict: %s from '%s' replaces the one from '%s'", path.c_str(), name_of(cur_).c_str(), name_of(prev).c_str());
            it->second = {std::move(src), cur_};
            o_.infos_[cur_].files++;
            return;
        }
        o_.files_.emplace(path, ModOverlay::FileRec{std::move(src), cur_});
        o_.dirs_[dir].kids.push_back({name, false});
        o_.infos_[cur_].files++;
    }

    // A .stub marker: the game's entry (and the files of lower-priority mods) at `path` disappear.
    void hide(const std::string& path) {
        if (path == "/" || path.empty()) return;
        drop_file(path);
        if (o_.dirs_.count(path)) remove_subtree(path);
        remove_kid(parent_path(path), leaf_name(path));
        o_.hidden_[path] = cur_;
    }

    // Lists the game's folders the mods touch to classify the files, and drops entries whose type disagrees with the game's.
    void reconcile(const stdfs::path& base_root) {
        BaseLists base(base_root, o_);
        const std::vector<std::string> dirs = sorted_dirs();
        for (const std::string& d : dirs) {
            auto dit = o_.dirs_.find(d);
            if (dit == o_.dirs_.end()) continue;   // dropped by a type conflict above
            BaseLists::Dir& b = base.get(d);
            dit->second.base = b.exists;
            const std::vector<ModOverlay::Child> kids = dit->second.kids;
            std::unordered_map<std::string, std::string> seen_lower;
            for (const ModOverlay::Child& k : kids) {
                const std::string cp = join_path(d, k.name);
                // The mods against each other: Horizon would serve both spellings as two files.
                auto ins = seen_lower.emplace(lower(k.name), k.name);
                if (!ins.second && ins.first->second != k.name)
                    NXLOG_W("mods", "'%s' and '%s' in %s differ only in case; the game treats them as two different files", ins.first->second.c_str(),
                            k.name.c_str(), d.c_str());
                const bool base_hidden = o_.hidden_.count(cp) != 0;
                auto bh = b.exists && !base_hidden ? b.names.find(k.name) : b.names.end();
                const bool hit = b.exists && !base_hidden && bh != b.names.end();
                if (hit && bh->second.is_dir != k.is_dir) {
                    NXLOG_W("mods", "mod '%s': '%s' is a %s in the mod but a %s in the game; it is ignored", owner_name(cp, k).c_str(), cp.c_str(),
                            k.is_dir ? "folder" : "file", k.is_dir ? "file" : "folder");
                    if (k.is_dir) {
                        remove_subtree(cp);
                    } else {
                        const uint32_t m = o_.files_[cp].mod;
                        o_.infos_[m].files--;
                        o_.files_.erase(cp);
                    }
                    remove_kid(d, k.name);
                    continue;
                }
                if (!hit && b.exists && !base_hidden) {
                    const std::string* alt = b.alt_case(k.name);
                    if (alt) warn_case(owner_of(cp, k), cp, join_path(d, *alt));
                }
                if (!k.is_dir) {
                    const uint32_t m = o_.files_[cp].mod;
                    (hit ? o_.infos_[m].overrides : o_.infos_[m].additions)++;
                    ModFileChange ch;
                    ch.path = cp;
                    ch.replaces_game_file = hit;
                    ch.old_size = hit ? bh->second.size : 0;
                    ch.new_size = o_.files_[cp].src.size;
                    ch.mod = o_.infos_[m].name;
                    changes_.push_back(std::move(ch));
                    NXLOG_D("mods", "mod '%s': %s %s", o_.infos_[m].name.c_str(), hit ? "replaces" : "adds", cp.c_str());
                }
            }
        }
        for (const auto& h : o_.hidden_) {
            const std::string par = parent_path(h.first), nm = leaf_name(h.first);
            BaseLists::Dir& b = base.get(par);
            if (b.exists && b.names.count(nm)) {
                o_.infos_[h.second].hides++;
                continue;
            }
            const std::string alt = base.resolve_alt(h.first);
            if (!alt.empty())
                NXLOG_W("mods", "mod '%s': the marker for '%s' hides nothing: the game's file is '%s' (names must match exactly, including case)",
                        o_.infos_[h.second].name.c_str(), h.first.c_str(), alt.c_str());
            else
                NXLOG_D("mods", "mod '%s': the marker for '%s' matches nothing in the game", o_.infos_[h.second].name.c_str(), h.first.c_str());
        }
        for (const auto& kv : case_extra_)
            NXLOG_W("mods", "mod '%s': %zu more names differ from the game's only in case (not listed)", o_.infos_[kv.first].name.c_str(), kv.second);
    }

    std::vector<ModFileChange> take_changes() {
        std::sort(changes_.begin(), changes_.end(), [](const ModFileChange& a, const ModFileChange& b) { return a.path < b.path; });
        return std::move(changes_);
    }

    // A file whose content a game module generated (ModsIndexEdit::replace_file). False when `path` names a folder.
    bool put_generated(const std::string& path, std::shared_ptr<const std::vector<uint8_t>> data) {
        if (path.size() < 2 || path[0] != '/' || path.size() > kRomfsMaxPath || leaf_name(path).size() > kRomfsMaxName) return false;
        if (o_.dirs_.count(path)) return false;
        const std::string dir = parent_path(path), name = leaf_name(path);
        ensure_dir(dir, std::wstring());
        ModSource src;
        src.size = data->size();
        src.mem = std::move(data);
        auto it = o_.files_.find(path);
        if (it != o_.files_.end()) {
            it->second = {std::move(src), kGenerated};
        } else {
            o_.files_.emplace(path, ModOverlay::FileRec{std::move(src), kGenerated});
            o_.dirs_[dir].kids.push_back({name, false});
        }
        return true;
    }

    void finish() {
        o_.dir_list_ = sorted_dirs();
        o_.hidden_list_.clear();
        for (const auto& h : o_.hidden_) o_.hidden_list_.push_back(h.first);
        std::sort(o_.hidden_list_.begin(), o_.hidden_list_.end());
    }

    void log_conflicts() const {
        std::vector<std::pair<uint64_t, const Conflict*>> v;
        for (const auto& kv : conflicts_) v.push_back({kv.first, &kv.second});
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (const auto& c : v) {
            const uint32_t hi = static_cast<uint32_t>(c.first >> 32), lo = static_cast<uint32_t>(c.first & 0xFFFFFFFFu);
            NXLOG_W("mods", "conflict: '%s' (higher in mods.ini) wins over '%s' on %zu file%s, e.g. %s", name_of(hi).c_str(), name_of(lo).c_str(),
                    c.second->count, c.second->count == 1 ? "" : "s", c.second->example.c_str());
        }
    }

private:
    struct Conflict {
        size_t count = 0;
        std::string example;
    };

    // The game's folders, listed once each.
    class BaseLists {
    public:
        struct Dir {
            bool exists = false;
            struct BaseEnt {
                bool is_dir;
                uint64_t size;
            };
            std::unordered_map<std::string, BaseEnt> names;
            std::unordered_map<std::string, std::string> by_lower;
            bool lower_built = false;
            const std::string* alt_case(const std::string& name) {
                if (!lower_built) {
                    for (const auto& n : names) by_lower.emplace(lower(n.first), n.first);
                    lower_built = true;
                }
                auto it = by_lower.find(lower(name));
                return it != by_lower.end() && it->second != name ? &it->second : nullptr;
            }
        };
        BaseLists(const stdfs::path& root, const ModOverlay& o) : root_(root), o_(o) {}
        // The game's folder `dir`, matched case-sensitively: the host file system is not, so each part must be in its parent's listing.
        Dir& get(const std::string& dir) {
            auto it = cache_.find(dir);
            if (it != cache_.end()) return it->second;
            bool exists = false;
            std::vector<Ent> ents;
            if (!o_.hidden(dir)) {   // a hidden game folder is not there
                bool parent_ok = true;
                if (dir != "/") {
                    Dir& par = get(parent_path(dir));
                    auto n = par.exists ? par.names.find(leaf_name(dir)) : par.names.end();
                    parent_ok = par.exists && n != par.names.end() && n->second.is_dir;
                }
                if (parent_ok) {
                    stdfs::path p = root_;
                    if (dir.size() > 1) p /= u8(dir.substr(1));
                    exists = list_entries(p, ents);
                }
            }
            Dir& d = cache_[dir];
            d.exists = exists;
            for (const Ent& e : ents) d.names.emplace(e.name, Dir::BaseEnt{e.dir, e.size});
            return d;
        }
        // The game's spelling of `path` when it exists only with other capitalisation somewhere; empty otherwise.
        std::string resolve_alt(const std::string& path) {
            std::string cur = "/";
            bool differed = false;
            size_t i = 1;
            while (i < path.size()) {
                const size_t j = path.find('/', i);
                const std::string comp = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
                Dir& d = get(cur);
                if (!d.exists) return std::string();
                std::string use = comp;
                if (d.names.find(comp) == d.names.end()) {
                    const std::string* alt = d.alt_case(comp);
                    if (!alt) return std::string();
                    use = *alt;
                    differed = true;
                }
                cur = join_path(cur, use);
                i = j == std::string::npos ? path.size() : j + 1;
            }
            return differed ? cur : std::string();
        }

    private:
        stdfs::path root_;
        const ModOverlay& o_;
        std::unordered_map<std::string, Dir> cache_;
    };

    std::string name_of(uint32_t m) const { return m < o_.infos_.size() ? o_.infos_[m].name : std::string("?"); }
    uint32_t owner_of(const std::string& cp, const ModOverlay::Child& k) const {
        if (!k.is_dir) {
            auto it = o_.files_.find(cp);
            if (it != o_.files_.end()) return it->second.mod;
        }
        return cur_;
    }
    std::string owner_name(const std::string& cp, const ModOverlay::Child& k) const { return name_of(owner_of(cp, k)); }

    void warn_case(uint32_t mod, const std::string& mine, const std::string& game) {
        size_t& n = case_count_[mod];
        if (n++ < kCaseWarnPerMod)
            NXLOG_W("mods", "mod '%s': '%s' differs from the game's '%s' only in case; paths are case-sensitive, so it is added as a separate file "
                            "and the game's one is not replaced (rename it to match)",
                    name_of(mod).c_str(), mine.c_str(), game.c_str());
        else
            case_extra_[mod]++;
    }

    std::vector<std::string> sorted_dirs() const {
        std::vector<std::string> v;
        v.reserve(o_.dirs_.size());
        for (const auto& kv : o_.dirs_) v.push_back(kv.first);
        std::sort(v.begin(), v.end());
        return v;
    }

    void remove_kid(const std::string& dir, const std::string& name) {
        auto it = o_.dirs_.find(dir);
        if (it == o_.dirs_.end()) return;
        auto& k = it->second.kids;
        k.erase(std::remove_if(k.begin(), k.end(), [&](const ModOverlay::Child& c) { return c.name == name; }), k.end());
    }

    void drop_file(const std::string& path) {
        auto it = o_.files_.find(path);
        if (it == o_.files_.end()) return;
        if (it->second.mod < o_.infos_.size()) {   // not a generated file
            if (it->second.mod != cur_) o_.infos_[it->second.mod].lost++;
            else o_.infos_[it->second.mod].files--;
        }
        o_.files_.erase(it);
    }

    // Removes a folder and everything below it from the mods' view; the files count as lost for their mods.
    void remove_subtree(const std::string& path) {
        const std::string prefix = path + "/";
        for (auto it = o_.files_.begin(); it != o_.files_.end();) {
            if (it->first.compare(0, prefix.size(), prefix) == 0) {
                if (it->second.mod < o_.infos_.size()) {
                    if (it->second.mod != cur_) o_.infos_[it->second.mod].lost++;
                    else o_.infos_[it->second.mod].files--;
                }
                it = o_.files_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = o_.dirs_.begin(); it != o_.dirs_.end();) {
            if (it->first == path || it->first.compare(0, prefix.size(), prefix) == 0) it = o_.dirs_.erase(it);
            else ++it;
        }
    }

    static constexpr uint32_t kGenerated = 0xFFFFFFFFu;   // FileRec::mod of a generated file
    ModOverlay& o_;
    uint32_t cur_ = 0;
    std::vector<ModFileChange> changes_;
    std::map<uint64_t, Conflict> conflicts_;
    std::map<uint32_t, size_t> case_count_, case_extra_;
};

namespace {

struct ScanStats {
    size_t invalid = 0;
    std::string invalid_example;
};

void note_invalid(ScanStats& st, const std::string& what) {
    if (st.invalid++ == 0) st.invalid_example = what;
}

// Walks a mod's romfs folder into the index.
void scan_loose(OverlayBuilder& b, const stdfs::path& dir, const std::string& img_dir, int depth, ScanStats& st) {
    b.ensure_dir(img_dir, ready_path(dir));
    if (depth > kMaxScanDepth) {
        note_invalid(st, img_dir + " (too deep)");
        return;
    }
    std::vector<Ent> ents;
    size_t bad = 0;
    list_entries(dir, ents, &bad);
    if (bad) note_invalid(st, path_u8(dir) + " (a name is not valid UTF-8)");
    for (const Ent& e : ents) {
        const std::string p = join_path(img_dir, e.name);
        if (e.name.size() > kRomfsMaxName || p.size() > kRomfsMaxPath) {
            note_invalid(st, p);
            continue;
        }
        if (e.dir) scan_loose(b, e.path, p, depth + 1, st);
        else if (e.reg) b.add_file(img_dir, e.name, ModSource{ready_path(e.path), 0, e.size});
    }
}

// romfs_ext: <name>.stub markers hide game entries (Yuzu/Eden). Returns true when the folder held .ips patches (not supported).
bool scan_ext(OverlayBuilder& b, const stdfs::path& dir, const std::string& img_dir, int depth, ScanStats& st) {
    if (depth > kMaxScanDepth) return false;
    std::vector<Ent> ents;
    list_entries(dir, ents);
    bool ips = false;
    for (const Ent& e : ents) {
        if (e.dir) {
            ips |= scan_ext(b, e.path, join_path(img_dir, e.name), depth + 1, st);
        } else if (e.reg && ends_with_i(e.name, ".stub") && e.name.size() > 5) {
            const std::string target = join_path(img_dir, e.name.substr(0, e.name.size() - 5));
            if (target.size() > kRomfsMaxPath) note_invalid(st, target);
            else b.hide(target);
        } else if (e.reg && ends_with_i(e.name, ".ips")) {
            ips = true;
        }
    }
    return ips;
}

}  // namespace

// ---- packed RomFS images -----------------------------------------------------------------------------------------------------------

bool read_packed_romfs(const std::wstring& file, std::vector<PackedEntry>* files, std::vector<std::string>* dirs, size_t* skipped, std::string* err) {
    files->clear();
    dirs->clear();
    *skipped = 0;
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("cannot be opened");
    struct Closer {
        HANDLE h;
        ~Closer() { CloseHandle(h); }
    } closer{h};
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) return fail("cannot be sized");
    const uint64_t fsize = static_cast<uint64_t>(li.QuadPart);
    auto rd = [&](uint64_t off, void* dst, uint64_t n) {
        uint8_t* out = static_cast<uint8_t*>(dst);
        while (n > 0) {
            const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(n, 1u << 28));
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFull);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            DWORD got = 0;
            if (!ReadFile(h, out, chunk, &got, &ov) || got != chunk) return false;
            out += chunk;
            off += chunk;
            n -= chunk;
        }
        return true;
    };
    auto le = [](const uint8_t* p, int n) {
        uint64_t v = 0;
        for (int i = 0; i < n; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
        return v;
    };
    uint8_t hb[0x50];
    if (fsize < sizeof hb || !rd(0, hb, sizeof hb)) return fail("is too small to be a RomFS image");
    uint64_t hdr[10];
    for (int i = 0; i < 10; i++) hdr[i] = le(hb + 8 * i, 8);
    if (hdr[0] != 0x50) return fail("does not start with a RomFS header");
    const uint64_t dir_ofs = hdr[3], dir_size = hdr[4], file_ofs = hdr[7], file_size = hdr[8], data_ofs = hdr[9];
    constexpr uint64_t kMaxTable = 1ull << 30;
    auto in_file = [&](uint64_t off, uint64_t n) { return off <= fsize && n <= fsize - off; };
    if (!in_file(dir_ofs, dir_size) || !in_file(file_ofs, file_size) || data_ofs > fsize || dir_size > kMaxTable || file_size > kMaxTable ||
        dir_size < 0x18)
        return fail("has table offsets outside the file (not a complete RomFS image)");
    std::vector<uint8_t> dm(static_cast<size_t>(dir_size)), fm(static_cast<size_t>(file_size));
    if (!rd(dir_ofs, dm.data(), dm.size()) || (!fm.empty() && !rd(file_ofs, fm.data(), fm.size()))) return fail("cannot be read");

    constexpr uint32_t kNone = 0xFFFFFFFFu;
    struct Item {
        uint32_t ofs;
        std::string path;
    };
    std::vector<Item> stack{{0, ""}};
    size_t dirs_seen = 0, file_budget = fm.size() / 0x20 + 1;
    const size_t dir_limit = dm.size() / 0x18 + 1;
    while (!stack.empty()) {
        Item it = std::move(stack.back());
        stack.pop_back();
        if (it.ofs > dm.size() || dm.size() - it.ofs < 0x18) return fail("has a folder entry outside its table");
        const uint8_t* d = dm.data() + it.ofs;
        const uint32_t child = static_cast<uint32_t>(le(d + 8, 4)), first_file = static_cast<uint32_t>(le(d + 12, 4));
        for (uint32_t f = first_file; f != kNone;) {
            if (file_budget-- == 0) return fail("has a loop in its file table");
            if (f > fm.size() || fm.size() - f < 0x20) return fail("has a file entry outside its table");
            const uint8_t* e = fm.data() + f;
            const uint64_t doff = le(e + 8, 8), dsize = le(e + 16, 8);
            const uint32_t nlen = static_cast<uint32_t>(le(e + 28, 4));
            const uint32_t next = static_cast<uint32_t>(le(e + 4, 4));
            if (nlen > fm.size() - f - 0x20) return fail("has a file name outside its table");
            std::string name(reinterpret_cast<const char*>(e + 0x20), nlen);
            const uint64_t abs = data_ofs + doff;
            if (name.empty() || name.size() > kRomfsMaxName || name.find('/') != std::string::npos || doff > fsize || !in_file(abs, dsize) ||
                it.path.size() + 1 + name.size() > kRomfsMaxPath) {
                ++*skipped;
            } else {
                files->push_back({it.path + "/" + name, abs, dsize});
            }
            f = next;
        }
        for (uint32_t c = child; c != kNone;) {
            if (++dirs_seen > dir_limit) return fail("has a loop in its folder table");
            if (c > dm.size() || dm.size() - c < 0x18) return fail("has a folder entry outside its table");
            const uint8_t* e = dm.data() + c;
            const uint32_t nlen = static_cast<uint32_t>(le(e + 20, 4));
            const uint32_t next = static_cast<uint32_t>(le(e + 4, 4));
            if (nlen > dm.size() - c - 0x18) return fail("has a folder name outside its table");
            std::string name(reinterpret_cast<const char*>(e + 0x18), nlen);
            if (name.empty() || name.size() > kRomfsMaxName || name.find('/') != std::string::npos ||
                it.path.size() + 1 + name.size() > kRomfsMaxPath) {
                ++*skipped;
            } else {
                dirs->push_back(it.path + "/" + name);
                stack.push_back({c, it.path + "/" + name});
            }
            c = next;
        }
    }
    return true;
}

// ---- configuration -----------------------------------------------------------------------------------------------------------------

ModsConfig mods_config_from_runtime() {
    ModsConfig c;
    c.enabled = config_get_bool("mods", "enabled", true);
    const stdfs::path ed = exe_dir();
    const std::string d = config_get_str("mods", "dir", "");
    stdfs::path dir = ed / "mods";
    if (!d.empty()) {
        const stdfs::path p = u8(d);
        dir = p.is_absolute() ? p : ed / p;
    }
    c.mods_dir = path_u8(dir.lexically_normal());
    const std::string ini = config_get_str("mods", "ini", "");
    if (ini.empty()) {
        c.ini_path = path_u8(ed / "mods.ini");
    } else {
        const stdfs::path p = u8(ini);
        c.ini_path = path_u8((p.is_absolute() ? p : ed / p).lexically_normal());
    }
    c.title_id = program_id_now();
    return c;
}

// ---- the hook for game modules -----------------------------------------------------------------------------------------------------

namespace {

std::mutex g_hook_mu;
ModsIndexHook g_hook;

// Reads [off, off + size) of a host file (to the end when to_end).
bool read_host_range(const std::wstring& host, uint64_t off, uint64_t size, bool to_end, std::vector<uint8_t>* out) {
    HANDLE h = CreateFileW(host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    struct Closer {
        HANDLE h;
        ~Closer() { CloseHandle(h); }
    } closer{h};
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) return false;
    const uint64_t fsize = static_cast<uint64_t>(li.QuadPart);
    if (off > fsize) return false;
    if (to_end) size = fsize - off;
    if (size > fsize - off || size > (1ull << 31)) return false;
    out->resize(static_cast<size_t>(size));
    uint8_t* p = out->data();
    while (size > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(size, 1u << 28));
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD got = 0;
        if (!ReadFile(h, p, chunk, &got, &ov) || got != chunk) return false;
        p += chunk;
        off += chunk;
        size -= chunk;
    }
    return true;
}

}  // namespace

void set_mods_index_hook(ModsIndexHook hook) {
    std::lock_guard<std::mutex> lk(g_hook_mu);
    g_hook = std::move(hook);
}

ModsIndexEdit::ModsIndexEdit(ModOverlay* o, OverlayBuilder* b, std::string base_root, std::vector<ModFileChange> changes)
    : o_(o), b_(b), base_root_(std::move(base_root)), changes_(std::move(changes)) {
    for (const auto& h : o_->hidden_) hidden_.push_back(h.first);
    std::sort(hidden_.begin(), hidden_.end());
}

bool ModsIndexEdit::read_file(const std::string& path, std::vector<uint8_t>* out) const {
    auto it = o_->files_.find(path);
    if (it != o_->files_.end()) {
        const ModSource& s = it->second.src;
        if (s.mem) {
            *out = *s.mem;
            return true;
        }
        return read_host_range(s.host, s.offset, s.size, false, out);
    }
    if (path.size() < 2 || path[0] != '/' || o_->hidden(path)) return false;
    return read_host_range(ready_path(u8(base_root_) / u8(path.substr(1))), 0, 0, true, out);
}

bool ModsIndexEdit::replace_file(const std::string& path, std::vector<uint8_t> data) {
    if (path.size() < 2 || path[0] != '/') return false;
    // The path must not be a folder, nor sit below a file, in the game's tree or in the mods'.
    std::error_code ec;
    if (stdfs::is_directory(u8(base_root_) / u8(path.substr(1)), ec) && !o_->hidden(path)) return false;
    for (size_t i = path.find('/', 1); i != std::string::npos; i = path.find('/', i + 1)) {
        const std::string anc = path.substr(0, i);
        if (o_->files_.count(anc)) return false;
        if (!o_->hidden(anc) && stdfs::is_regular_file(u8(base_root_) / u8(anc.substr(1)), ec)) return false;
    }
    if (!b_->put_generated(path, std::make_shared<const std::vector<uint8_t>>(std::move(data)))) return false;
    replaced_++;
    return true;
}

// ---- load_mods ---------------------------------------------------------------------------------------------------------------------

std::shared_ptr<const ModOverlay> load_mods(const ModsConfig& cfg, const std::string& base_romfs_dir) {
    if (!cfg.enabled) {
        NXLOG_I("mods", "mods are switched off ([mods] enabled = false)");
        return nullptr;
    }
    std::error_code ec;
    const stdfs::path root = u8(cfg.mods_dir);
    if (cfg.mods_dir.empty() || !stdfs::is_directory(root, ec)) {
        NXLOG_D("mods", "no mods folder at '%s'", cfg.mods_dir.c_str());
        return nullptr;
    }

    Discovery d = discover(root, cfg.title_id);
    if (d.mods.empty()) {
        if (!d.skipped_other_title && d.global_unsupported.empty())
            NXLOG_I("mods", "no mods in '%s' (a mod is a folder like mods/<name>/romfs/...; see docs/MODS.md)", cfg.mods_dir.c_str());
        else if (!d.global_unsupported.empty())
            NXLOG_W("mods", "no mods with RomFS content in '%s'; the code patch folder%s (%s) cannot be used by a recompiled game", cfg.mods_dir.c_str(),
                    d.global_unsupported.size() == 1 ? "" : "s", d.global_unsupported[0].c_str());
        return nullptr;
    }

    // Order and enabled flags: mods.ini first (in its order), then the mods it does not know yet, appended on.
    ModsIni ini = read_ini(cfg.ini_path);
    struct Plan {
        size_t found;
        bool on;
    };
    std::vector<Plan> plan;
    std::vector<bool> used(d.mods.size(), false);
    for (const ModsIni::Entry& e : ini.entries) {
        for (size_t i = 0; i < d.mods.size(); i++) {
            if (!used[i] && ieq(d.mods[i].name, e.name)) {
                used[i] = true;
                plan.push_back({i, e.on});
                break;
            }
        }
    }
    std::vector<std::string> added;
    for (size_t i = 0; i < d.mods.size(); i++) {
        if (used[i]) continue;
        plan.push_back({i, true});
        added.push_back(d.mods[i].name);
    }
    if (!added.empty() && !cfg.ini_path.empty()) {
        std::string list;
        for (const std::string& a : added) list += (list.empty() ? "" : ", ") + a;
        if (write_ini(cfg.ini_path, ini, added))
            NXLOG_I("mods", "new mod%s added to mods.ini (on, lowest priority): %s", added.size() == 1 ? "" : "s", list.c_str());
        else
            NXLOG_W("mods", "mods.ini cannot be written ('%s'); new mods (%s) are on for this run and listed after the others", cfg.ini_path.c_str(), list.c_str());
    }

    auto ov = std::make_shared<ModOverlay>();
    OverlayBuilder b(*ov);
    for (const Plan& p : plan) {
        ModOverlay::ModInfo mi;
        mi.name = d.mods[p.found].name;
        mi.enabled = p.on;
        mi.folder = d.mods[p.found].folder;
        mi.unsupported = d.mods[p.found].unsupported;
        ov->infos_.push_back(std::move(mi));
    }

    // Lowest priority first, so that a later mod replaces what an earlier one put in.
    std::vector<std::string> bad_notes;
    for (size_t pi = plan.size(); pi-- > 0;) {
        if (!plan[pi].on) continue;
        const FoundMod& m = d.mods[plan[pi].found];
        b.begin_mod(static_cast<uint32_t>(pi));
        ScanStats st;
        bool ips = false;
        for (const stdfs::path& e : m.ext_dirs) ips |= scan_ext(b, e, "/", 0, st);
        for (const stdfs::path& bin : m.bins) {
            std::vector<PackedEntry> files;
            std::vector<std::string> dirs;
            size_t skipped = 0;
            std::string err;
            const std::wstring host = ready_path(bin);
            if (!read_packed_romfs(host, &files, &dirs, &skipped, &err)) {
                NXLOG_W("mods", "mod '%s': '%s' is not usable: it %s; ignored", m.name.c_str(), path_u8(bin).c_str(), err.c_str());
                continue;
            }
            for (const std::string& dpath : dirs) b.ensure_dir(dpath, host);
            for (const PackedEntry& f : files) {
                const std::string dir = parent_path(f.path);
                b.ensure_dir(dir, host);
                b.add_file(dir, leaf_name(f.path), ModSource{host, f.offset, f.size});
            }
            if (skipped) note_invalid(st, path_u8(bin) + " (" + std::to_string(skipped) + " damaged or oversized entries)");
        }
        for (const stdfs::path& r : m.romfs_dirs) scan_loose(b, r, "/", 0, st);
        if (ips) ov->infos_[pi].unsupported.push_back("romfs_ext .ips");
        if (st.invalid)
            NXLOG_W("mods", "mod '%s': %zu entr%s skipped (name over %zu bytes, path over %zu bytes, or damaged), e.g. %s", m.name.c_str(), st.invalid,
                    st.invalid == 1 ? "y" : "ies", kRomfsMaxName, kRomfsMaxPath, st.invalid_example.c_str());
    }
    b.reconcile(u8(base_romfs_dir));

    // The report: one line per mod in priority order, then conflicts, then what cannot be used.
    size_t on = 0, off = 0, served = 0, over = 0, add = 0, hides = 0;
    std::string unsupported;
    for (const ModOverlay::ModInfo& mi : ov->infos_) {
        if (!mi.enabled) {
            off++;
            NXLOG_I("mods", "mod '%s': off in mods.ini", mi.name.c_str());
            continue;
        }
        on++;
        served += mi.overrides + mi.additions;
        over += mi.overrides;
        add += mi.additions;
        hides += mi.hides;
        if (mi.files == 0 && mi.hides == 0) {
            if (mi.unsupported.empty())
                NXLOG_W("mods", "mod '%s' (%s): its romfs folder holds no files; nothing to load", mi.name.c_str(), mi.folder.c_str());
            else
                NXLOG_I("mods", "mod '%s': no RomFS content (only %s); nothing to load", mi.name.c_str(), mi.unsupported[0].c_str());
        } else {
            std::string extra;
            if (mi.lost) extra += ", " + std::to_string(mi.lost) + " beaten by higher-priority mods";
            if (mi.hides) extra += ", hides " + std::to_string(mi.hides) + " game entr" + (mi.hides == 1 ? "y" : "ies");
            NXLOG_I("mods", "mod '%s': %zu file%s (%zu replace game files, %zu new)%s", mi.name.c_str(), mi.files, mi.files == 1 ? "" : "s", mi.overrides,
                    mi.additions, extra.c_str());
        }
        if (!mi.unsupported.empty()) {
            std::string what;
            for (const std::string& u : mi.unsupported) what += (what.empty() ? "" : ", ") + u;
            unsupported += (unsupported.empty() ? "" : "; ") + ("'" + mi.name + "' [" + what + "]");
        }
    }
    b.log_conflicts();
    if (!unsupported.empty() || !d.global_unsupported.empty()) {
        for (const std::string& g : d.global_unsupported) unsupported += (unsupported.empty() ? "" : "; ") + ("mods/" + g);
        NXLOG_W("mods",
                "code patches, cheats and file patches are NOT supported and are ignored: %s. A recompiled game has no ExeFS code to patch "
                "(exefs, exefs_patches, IPS/pchtxt) and cheats and romfs_ext .ips patches are not implemented; the RomFS files of these mods still load",
                unsupported.c_str());
    }
    NXLOG_I("mods", "%zu mod%s on, %zu off: %zu RomFS file%s served from mods (%zu replace game files, %zu new)%s", on, on == 1 ? "" : "s", off, served,
            served == 1 ? "" : "s", over, add, hides ? (", " + std::to_string(hides) + " game entries hidden").c_str() : "");
    if (ov->empty()) return nullptr;
    ModsIndexHook hook;
    {
        std::lock_guard<std::mutex> lk(g_hook_mu);
        hook = g_hook;
    }
    if (hook) {   // the game module may fix up tables that depend on the files served (ModsIndexEdit)
        ModsIndexEdit edit(ov.get(), &b, base_romfs_dir, b.take_changes());
        try {
            hook(edit);
        } catch (const std::exception& ex) {
            NXLOG_W("mods", "the game module's mod hook failed: %s", ex.what());
        } catch (...) {
            NXLOG_W("mods", "the game module's mod hook failed");
        }
        if (edit.replaced())
            NXLOG_I("mods", "the game module generated %zu file%s for the merged tree", edit.replaced(), edit.replaced() == 1 ? "" : "s");
    }
    b.finish();
    return ov;
}

}  // namespace nxrt::services::fs
