// SPDX-License-Identifier: GPL-3.0-or-later
// Mod loading for the RomFS (services/fs_mods): discovery of the folder layouts Atmosphere, Ryujinx and Eden use, mods.ini order and
// enabled flags, overrides, additions, priorities, .stub markers, packed romfs.bin images, listing merges, and the no-mods path. The
// merged tree is read back two ways: through a RomFS image parsed with the same hash tables and sibling chains the SDK uses, and through
// the read-only IFileSystem volume; the two must agree. Fixture trees are made under the temp folder for each test and removed at the end.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/services/fs_mods.h"
#include "../src/services/fs_romfs.h"
#include "../src/services/fs_volume.h"
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "svc_test_util.h"
#include "test_framework.h"

NXTEST_GROUP(svc_fs_mods)

namespace {

using namespace nxrt::services::fs;
using nxrt::LogLevel;
namespace stdfs = std::filesystem;

constexpr uint64_t kTitle = 0x01006F8002326000ull;
const std::string kTid = "01006F8002326000";

// ---- fixtures ----------------------------------------------------------------------------------------------------------------------

std::string u8s(const stdfs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

struct TempTree {
    stdfs::path root;
    TempTree() {
        static std::atomic<unsigned> seq{0};
        std::error_code ec;
        root = stdfs::temp_directory_path(ec) / ("nxrt_mods_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(seq++));
        stdfs::create_directories(root, ec);
    }
    ~TempTree() {
        std::error_code ec;
        stdfs::remove_all(root, ec);
    }
};

void put(const stdfs::path& root, const std::string& rel, const std::string& data) {
    const stdfs::path p = root / stdfs::u8path(rel);
    std::error_code ec;
    stdfs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    CHECK_MSG(f.good(), rel);
}

std::string slurp(const stdfs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The game's RomFS folder, the mods folder and mods.ini of one test.
struct Env {
    TempTree t;
    stdfs::path base, mods, ini;
    uint64_t title = kTitle;
    bool enabled = true;
    Env() {
        base = t.root / "game" / "romfs";
        mods = t.root / "mods";
        ini = t.root / "mods.ini";
        std::error_code ec;
        stdfs::create_directories(base, ec);
        stdfs::create_directories(mods, ec);
    }
    void game(const std::string& rel, const std::string& data) { put(base, rel, data); }
    void mod(const std::string& rel, const std::string& data = "x") { put(mods, rel, data); }
    void set_ini(const std::string& text) {
        std::ofstream f(ini, std::ios::binary | std::ios::trunc);
        f << text;
    }
    ModsConfig cfg() const {
        ModsConfig c;
        c.enabled = enabled;
        c.mods_dir = u8s(mods);
        c.ini_path = u8s(ini);
        c.title_id = title;
        return c;
    }
    std::shared_ptr<const ModOverlay> load() const { return load_mods(cfg(), u8s(base)); }
    std::string base_s() const { return u8s(base); }
};

// Log lines of a test, with their levels.
struct Cap {
    std::mutex mu;
    std::vector<std::pair<LogLevel, std::string>> lines;
    LogLevel old;
    static void sink(LogLevel l, const char* line, void* user) {
        auto* c = static_cast<Cap*>(user);
        std::lock_guard<std::mutex> lk(c->mu);
        c->lines.emplace_back(l, line);
    }
    explicit Cap(LogLevel level = LogLevel::Info) {
        old = nxrt::log_level();
        nxrt::log_set_level(level);
        nxrt::log_set_sink(&Cap::sink, this);
    }
    ~Cap() {
        nxrt::log_set_sink(nullptr, nullptr);
        nxrt::log_set_level(old);
    }
    // Lines at `level` (or any level when `any`) that contain every needle.
    int count(std::initializer_list<const char*> needles, LogLevel level, bool all_levels = false) {
        std::lock_guard<std::mutex> lk(mu);
        int n = 0;
        for (const auto& l : lines) {
            if (!all_levels && l.first != level) continue;
            bool all = true;
            for (const char* s : needles) all = all && l.second.find(s) != std::string::npos;
            n += all;
        }
        return n;
    }
    int warn(std::initializer_list<const char*> needles) { return count(needles, LogLevel::Warn); }
    int info(std::initializer_list<const char*> needles) { return count(needles, LogLevel::Info); }
    int any(std::initializer_list<const char*> needles) { return count(needles, LogLevel::Info, true); }
    int total() {
        std::lock_guard<std::mutex> lk(mu);
        return static_cast<int>(lines.size());
    }
    std::string dump() {
        std::lock_guard<std::mutex> lk(mu);
        std::string s;
        for (const auto& l : lines) s += l.second + "\n";
        return s;
    }
};

// ---- reading an image the way the SDK does ----------------------------------------------------------------------------------------

constexpr uint32_t kNone = 0xFFFFFFFFu;

uint64_t le(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

uint32_t name_hash(uint32_t parent, const std::string& name) {
    uint32_t h = parent ^ 123456789u;
    for (unsigned char c : name) {
        h = (h >> 5) | (h << 27);
        h ^= c;
    }
    return h;
}

class Img {
public:
    explicit Img(const RomfsImage& i) : img_(i) {}
    bool load() {
        uint8_t h[0x50];
        if (!img_.built() || img_.size() < 0x50 || !img_.read(0, h, sizeof h)) return false;
        for (int i = 0; i < 10; i++) hdr_[i] = le(h + 8 * i, 8);
        base_ = hdr_[1];
        meta_.resize(static_cast<size_t>(img_.size() - base_));
        return img_.read(base_, meta_.data(), meta_.size());
    }
    // Hash-table lookups, as nn::fs does for open and get-entry-type.
    uint32_t find_dir(uint32_t parent, const std::string& name) const {
        const uint64_t nb = hdr_[2] / 4;
        uint32_t e = u32(4 * (name_hash(parent, name) % nb));
        const size_t dt = static_cast<size_t>(hdr_[3] - base_);
        for (int guard = 0; e != kNone && guard < 1000000; guard++) {
            const size_t o = dt + e;
            if (u32(o) == parent && u32(o + 20) == name.size() && std::memcmp(&meta_[o + 24], name.data(), name.size()) == 0) return e;
            e = u32(o + 16);
        }
        return kNone;
    }
    uint32_t find_file(uint32_t parent, const std::string& name) const {
        const uint64_t nb = hdr_[6] / 4;
        const size_t fh = static_cast<size_t>(hdr_[5] - base_), ft = static_cast<size_t>(hdr_[7] - base_);
        uint32_t e = u32(fh + 4 * (name_hash(parent, name) % nb));
        for (int guard = 0; e != kNone && guard < 1000000; guard++) {
            const size_t o = ft + e;
            if (u32(o) == parent && u32(o + 28) == name.size() && std::memcmp(&meta_[o + 32], name.data(), name.size()) == 0) return e;
            e = u32(o + 24);
        }
        return kNone;
    }
    bool dir_at(const std::string& path, uint32_t* off) const {
        *off = 0;
        size_t i = 1;
        while (i < path.size()) {
            const size_t j = path.find('/', i);
            *off = find_dir(*off, path.substr(i, j == std::string::npos ? std::string::npos : j - i));
            if (*off == kNone) return false;
            i = j == std::string::npos ? path.size() : j + 1;
        }
        return true;
    }
    // 0 = absent, 1 = directory, 2 = file
    int type(const std::string& path, uint64_t* size = nullptr) const {
        if (path == "/") return 1;
        const size_t cut = path.rfind('/');
        uint32_t par;
        if (!dir_at(cut == 0 ? "/" : path.substr(0, cut), &par)) return 0;
        const std::string leaf = path.substr(cut + 1);
        if (const uint32_t f = find_file(par, leaf); f != kNone) {
            if (size) *size = u64(static_cast<size_t>(hdr_[7] - base_) + f + 16);
            return 2;
        }
        return find_dir(par, leaf) != kNone ? 1 : 0;
    }
    // The children of a folder by walking the child and sibling chains (what a directory iterator does): name -> is_dir.
    bool list(const std::string& path, std::map<std::string, bool>* out) const {
        out->clear();
        uint32_t d;
        if (!dir_at(path, &d)) return false;
        const size_t dt = static_cast<size_t>(hdr_[3] - base_), ft = static_cast<size_t>(hdr_[7] - base_);
        int guard = 0;
        for (uint32_t c = u32(dt + d + 8); c != kNone && guard++ < 1000000; c = u32(dt + c + 4))
            (*out)[std::string(reinterpret_cast<const char*>(&meta_[dt + c + 24]), u32(dt + c + 20))] = true;
        for (uint32_t f = u32(dt + d + 12); f != kNone && guard++ < 1000000; f = u32(ft + f + 4))
            (*out)[std::string(reinterpret_cast<const char*>(&meta_[ft + f + 32]), u32(ft + f + 28))] = false;
        return true;
    }
    bool read(const std::string& path, std::string* out) const {
        const size_t cut = path.rfind('/');
        uint32_t par;
        if (cut == std::string::npos || !dir_at(cut == 0 ? "/" : path.substr(0, cut), &par)) return false;
        const uint32_t f = find_file(par, path.substr(cut + 1));
        if (f == kNone) return false;
        const size_t o = static_cast<size_t>(hdr_[7] - base_) + f;
        const uint64_t off = u64(o + 8), size = u64(o + 16);
        out->resize(static_cast<size_t>(size));
        return size == 0 || img_.read(hdr_[9] + off, out->data(), size);
    }
    std::string get(const std::string& path) const {
        std::string s;
        return read(path, &s) ? s : std::string("<absent>");
    }

private:
    uint32_t u32(size_t o) const { return static_cast<uint32_t>(le(&meta_[o], 4)); }
    uint64_t u64(size_t o) const { return le(&meta_[o], 8); }
    const RomfsImage& img_;
    uint64_t hdr_[10] = {};
    uint64_t base_ = 0;
    std::vector<uint8_t> meta_;
};

// Image plus its reader, built over the env's game folder with the given mods.
struct Built {
    RomfsImage img;
    Img r{img};
    bool ok = false;
    std::string err;
    Built(const Env& e, const ModOverlay* ov) {
        ok = img.build(e.base_s(), &err, ov) && r.load();
        CHECK_MSG(ok, err);
    }
};

// Everything the game can see, as path -> "D" or "F<size>:<content>", from the image and from the volume.
void walk_img(const Img& r, const std::string& dir, std::map<std::string, std::string>* out) {
    std::map<std::string, bool> kids;
    CHECK(r.list(dir, &kids));
    for (const auto& k : kids) {
        const std::string p = dir == "/" ? "/" + k.first : dir + "/" + k.first;
        if (k.second) {
            (*out)[p] = "D";
            walk_img(r, p, out);
        } else {
            (*out)[p] = "F" + std::to_string(r.get(p).size()) + ":" + r.get(p);
        }
    }
}

void walk_vol(Volume& v, const std::string& dir, std::map<std::string, std::string>* out) {
    std::vector<DirItem> items;
    CHECK_EQ(v.list_dir(dir, &items), 0u);
    for (const DirItem& it : items) {
        const std::string p = dir == "/" ? "/" + it.name : dir + "/" + it.name;
        uint32_t type = 9;
        CHECK_EQ(v.entry_type(p, &type), 0u);
        if (it.is_dir) {
            CHECK_EQ(type, 0u);
            (*out)[p] = "D";
            walk_vol(v, p, out);
            continue;
        }
        CHECK_EQ(type, 1u);
        std::shared_ptr<VFile> f;
        CHECK_EQ(v.open_file(p, kModeRead, &f), 0u);
        if (!f) return;
        int64_t size = -1;
        CHECK_EQ(f->get_size(&size), 0u);
        CHECK_EQ(size, it.size);
        std::string data(static_cast<size_t>(size), '\0');
        uint64_t got = 0;
        CHECK_EQ(f->read(0, data.data(), data.size(), &got), 0u);
        CHECK_EQ(got, data.size());
        (*out)[p] = "F" + std::to_string(data.size()) + ":" + data;
    }
}

// Both views of the same merge, compared. Returns the image-side map for further checks.
std::map<std::string, std::string> same_tree(const Env& e, const std::shared_ptr<const ModOverlay>& ov, const Built& b) {
    std::map<std::string, std::string> a, v;
    walk_img(b.r, "/", &a);
    Volume vol(e.base_s(), false, true, 0);
    CHECK(vol.ok());
    vol.set_overlay(ov);
    walk_vol(vol, "/", &v);
    CHECK_MSG(a == v, "image and volume trees differ");
    if (a != v) {
        for (const auto& kv : a)
            if (!v.count(kv.first) || v[kv.first] != kv.second) std::fprintf(stderr, "  differs: %s\n", kv.first.c_str());
        for (const auto& kv : v)
            if (!a.count(kv.first)) std::fprintf(stderr, "  only in volume: %s\n", kv.first.c_str());
    }
    return a;
}

std::set<std::string> mod_names(const std::shared_ptr<const ModOverlay>& ov) {
    std::set<std::string> s;
    if (ov)
        for (const auto& m : ov->mods()) s.insert(m.name);
    return s;
}

const ModOverlay::ModInfo* info_of(const std::shared_ptr<const ModOverlay>& ov, const std::string& name) {
    if (ov)
        for (const auto& m : ov->mods())
            if (m.name == name) return &m;
    return nullptr;
}

}  // namespace

// ---- overrides and additions ------------------------------------------------------------------------------------------------------

NXTEST(override_and_addition) {
    Env e;
    e.game("a.bin", "base-a");
    e.game("dir/b.bin", "base-b");
    e.game("keep.txt", "keep");
    e.mod("M/romfs/a.bin", "mod-a-longer");
    e.mod("M/romfs/new/c.bin", "mod-c");
    e.mod("M/romfs/dir/d.bin", "mod-d");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    const auto* m = info_of(ov, "M");
    REQUIRE(m != nullptr);
    CHECK_EQ(m->files, 3u);
    CHECK_EQ(m->overrides, 1u);
    CHECK_EQ(m->additions, 2u);
    Built b(e, ov.get());
    CHECK_EQ(b.r.get("/a.bin"), std::string("mod-a-longer"));
    CHECK_EQ(b.r.get("/dir/b.bin"), std::string("base-b"));
    CHECK_EQ(b.r.get("/dir/d.bin"), std::string("mod-d"));
    CHECK_EQ(b.r.get("/new/c.bin"), std::string("mod-c"));
    CHECK_EQ(b.r.get("/keep.txt"), std::string("keep"));
    uint64_t size = 0;
    CHECK_EQ(b.r.type("/a.bin", &size), 2);
    CHECK_EQ(size, 12u);
    CHECK_EQ(b.r.type("/new"), 1);
    CHECK_EQ(b.r.type("/nope.bin"), 0);
    same_tree(e, ov, b);
    // One line for the mod, none for single files.
    CHECK_EQ(cap.info({"mod 'M'", "3 files", "1 replace game files", "2 new"}), 1);
    CHECK_EQ(cap.any({"a.bin"}), 0);
    CHECK_EQ(cap.warn({""}), 0);
}

NXTEST(per_file_lines_only_at_debug_level) {
    Env e;
    e.game("a.bin", "base-a");
    e.mod("M/romfs/a.bin", "m");
    e.mod("M/romfs/b.bin", "m");
    Cap cap(LogLevel::Debug);
    auto ov = e.load();
    REQUIRE(ov);
    CHECK(cap.count({"replaces /a.bin"}, LogLevel::Debug) == 1);
    CHECK(cap.count({"adds /b.bin"}, LogLevel::Debug) == 1);
}

// ---- priority, mods.ini ------------------------------------------------------------------------------------------------------------

NXTEST(priority_follows_mods_ini_and_defaults_to_name_order) {
    Env e;
    e.game("x.bin", "base");
    e.mod("A/romfs/x.bin", "from-A");
    e.mod("B/romfs/x.bin", "from-B");
    e.mod("B/romfs/only-b.bin", "b");
    {
        Cap cap;
        auto ov = e.load();   // no mods.ini yet: A before B, so A wins
        REQUIRE(ov);
        Built b(e, ov.get());
        CHECK_EQ(b.r.get("/x.bin"), std::string("from-A"));
        CHECK_EQ(b.r.get("/only-b.bin"), std::string("b"));
        CHECK_EQ(cap.warn({"conflict", "'A'", "wins over 'B'", "1 file", "/x.bin"}), 1);
        CHECK_EQ(info_of(ov, "B")->lost, 1u);
        CHECK_EQ(info_of(ov, "A")->lost, 0u);
        // The list was written, A above B, both on.
        const std::string ini = slurp(e.ini);
        CHECK(ini.find("[order]") != std::string::npos);
        const size_t a = ini.find("A = on"), bb = ini.find("B = on");
        CHECK(a != std::string::npos && bb != std::string::npos && a < bb);
        CHECK_EQ(cap.info({"added to mods.ini"}), 1);
    }
    e.set_ini("[order]\r\nB = on\r\nA = on\r\n");
    {
        Cap cap;
        auto ov = e.load();
        REQUIRE(ov);
        Built b(e, ov.get());
        CHECK_EQ(b.r.get("/x.bin"), std::string("from-B"));
        CHECK_EQ(cap.warn({"conflict", "'B'", "wins over 'A'"}), 1);
        CHECK_EQ(cap.info({"added to mods.ini"}), 0);   // nothing new: the file is left alone
        CHECK_EQ(slurp(e.ini), std::string("[order]\r\nB = on\r\nA = on\r\n"));
    }
}

NXTEST(three_mods_chain_of_conflicts) {
    Env e;
    e.mod("A/romfs/x.bin", "A");
    e.mod("B/romfs/x.bin", "B");
    e.mod("C/romfs/x.bin", "C");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    Built b(e, ov.get());
    CHECK_EQ(b.r.get("/x.bin"), std::string("A"));
    CHECK_EQ(info_of(ov, "C")->lost, 1u);
    CHECK_EQ(info_of(ov, "B")->lost, 1u);
    CHECK_EQ(cap.warn({"conflict"}), 2);
    CHECK_EQ(cap.info({"mod 'B'", "beaten by higher-priority mods"}), 1);
}

NXTEST(disabled_mod_is_not_loaded) {
    Env e;
    e.game("x.bin", "base");
    e.mod("A/romfs/x.bin", "from-A");
    e.mod("A/romfs/y.bin", "y");
    e.mod("B/romfs/x.bin", "from-B");
    e.set_ini("[order]\nA = off\nB = on\n");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    Built b(e, ov.get());
    CHECK_EQ(b.r.get("/x.bin"), std::string("from-B"));
    CHECK_EQ(b.r.get("/y.bin"), std::string("<absent>"));
    CHECK(!info_of(ov, "A")->enabled);
    CHECK_EQ(cap.info({"mod 'A'", "off in mods.ini"}), 1);
    CHECK_EQ(cap.warn({"conflict"}), 0);   // a disabled mod is not in conflict with anything
}

NXTEST(enabled_flag_spellings) {
    const char* off[] = {"off", "OFF", "false", "no", "0", "disabled"};
    const char* on[] = {"on", "true", "Yes", "1", "enabled", ""};
    for (const char* v : off) {
        Env e;
        e.mod("A/romfs/x.bin", "a");
        e.set_ini(std::string("[order]\nA = ") + v + "\n");
        Cap cap;
        CHECK_MSG(e.load() == nullptr, v);
        CHECK_MSG(cap.warn({"mods.ini"}) == 0, v);
    }
    for (const char* v : on) {
        Env e;
        e.mod("A/romfs/x.bin", "a");
        e.set_ini(std::string("[order]\nA = ") + v + "\n");
        Cap cap;
        CHECK_MSG(e.load() != nullptr, v);
        CHECK_MSG(cap.warn({"mods.ini"}) == 0, v);
    }
    Env e;
    e.mod("A/romfs/x.bin", "a");
    e.set_ini("[order]\nA = maybe\nA = off\n");
    Cap cap;
    CHECK(e.load() != nullptr);   // an unknown word counts as on, and the first of two entries counts
    CHECK_EQ(cap.warn({"mods.ini line 2", "maybe"}), 1);
    CHECK_EQ(cap.warn({"mods.ini line 3", "listed twice"}), 1);
}

NXTEST(new_mods_join_enabled_at_the_bottom_and_comments_survive) {
    Env e;
    e.mod("A/romfs/x.bin", "from-A");
    e.mod("B/romfs/x.bin", "from-B");
    e.set_ini("; my notes\r\n[order]\r\nB = on\r\n; keep this\r\n");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    Built b(e, ov.get());
    CHECK_EQ(b.r.get("/x.bin"), std::string("from-B"));   // B is listed, A is new and below it
    const std::string ini = slurp(e.ini);
    CHECK(ini.find("; my notes") != std::string::npos);
    CHECK(ini.find("; keep this") != std::string::npos);
    const size_t pb = ini.find("B = on"), pa = ini.find("A = on");
    CHECK(pb != std::string::npos && pa != std::string::npos && pb < pa);
    CHECK_EQ(cap.info({"added to mods.ini", ": A"}), 1);
    // A second start changes nothing.
    auto ov2 = e.load();
    CHECK_EQ(slurp(e.ini), ini);
    REQUIRE(ov2);
}

NXTEST(stale_entries_stay_listed_and_unwritable_ini_is_not_fatal) {
    Env e;
    e.mod("A/romfs/x.bin", "a");
    e.set_ini("[order]\nGone = on\nA = on\n");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(mod_names(ov).size(), 1u);
    CHECK(slurp(e.ini).find("Gone = on") != std::string::npos);   // a mod that is moved away keeps its place
    // mods.ini in a folder that does not exist: the mod still loads, with one warning.
    Env f;
    f.mod("A/romfs/x.bin", "a");
    f.ini = f.t.root / "no_such_folder" / "mods.ini";
    auto ov2 = f.load();
    CHECK(ov2 != nullptr);
    CHECK_EQ(cap.warn({"mods.ini cannot be written"}), 1);
}

NXTEST(mods_ini_names_ignore_case_and_separator_style) {
    Env e;
    e.mod("Pack/Sub/romfs/x.bin", "sub");
    e.mod("Other/romfs/y.bin", "y");
    e.set_ini("[mods]\r\n  pack\\SUB  =  OFF  \r\nOther=on\r\n");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK(info_of(ov, "Pack/Sub") != nullptr);
    CHECK(!info_of(ov, "Pack/Sub")->enabled);
    CHECK(info_of(ov, "Other")->enabled);
    CHECK_EQ(cap.info({"added to mods.ini"}), 0);   // both were recognised, none is "new"
}

// ---- directory listings ----------------------------------------------------------------------------------------------------------

NXTEST(directory_listings_merge) {
    Env e;
    e.game("dir/b1.bin", "base-1");
    e.game("dir/b2.bin", "base-2");
    e.game("dir/sub/deep.bin", "deep");
    e.mod("M/romfs/dir/b2.bin", "mod-2-bigger");
    e.mod("M/romfs/dir/d1.bin", "mod-d1");
    e.mod("M/romfs/dir/sub/more.bin", "more");
    e.mod("M/romfs/onlymod/z.bin", "z");
    auto ov = e.load();
    REQUIRE(ov);
    Built b(e, ov.get());
    std::map<std::string, bool> kids;
    CHECK(b.r.list("/dir", &kids));
    CHECK_EQ(kids.size(), 4u);
    CHECK(kids.count("b1.bin") && !kids["b1.bin"]);
    CHECK(kids.count("b2.bin") && !kids["b2.bin"]);
    CHECK(kids.count("d1.bin") && !kids["d1.bin"]);
    CHECK(kids.count("sub") && kids["sub"]);
    CHECK(b.r.list("/dir/sub", &kids));
    CHECK_EQ(kids.size(), 2u);
    CHECK(b.r.list("/", &kids));
    CHECK(kids.count("onlymod") && kids["onlymod"]);
    CHECK_EQ(b.r.get("/dir/b2.bin"), std::string("mod-2-bigger"));
    // The volume's listing is sorted and carries the mod's sizes.
    Volume v(e.base_s(), false, true, 0);
    v.set_overlay(ov);
    std::vector<DirItem> items;
    CHECK_EQ(v.list_dir("/dir", &items), 0u);
    REQUIRE(items.size() == 4);
    CHECK_EQ(items[0].name, std::string("b1.bin"));
    CHECK_EQ(items[1].name, std::string("b2.bin"));
    CHECK_EQ(items[1].size, 12);
    uint32_t type = 9;
    CHECK_EQ(v.entry_type("/onlymod", &type), 0u);
    CHECK_EQ(type, 0u);
    CHECK_EQ(v.entry_type("/onlymod/nothing", &type), rc::kPathNotFound);
    same_tree(e, ov, b);
}

// ---- layouts -----------------------------------------------------------------------------------------------------------------------

NXTEST(every_layout_the_emulators_and_atmosphere_use) {
    Env e;
    e.game("base.bin", "base");
    e.mod("AtmoA/atmosphere/contents/" + kTid + "/romfs/f1.bin", "1");
    e.mod("AtmoB/contents/01006f8002326000/romfs/f2.bin", "2");          // lower-case title id
    e.mod("atmosphere/contents/" + kTid + "/romfs/f3.bin", "3");          // a zip extracted straight into mods/
    e.mod("contents/" + kTid + "/RyuMod/romfs/f4.bin", "4");              // Ryujinx's mods folder
    e.mod(kTid + "/EdenMod/romfs/f5.bin", "5");                           // Eden's load folder
    e.mod("CaseMod/RomFS/f6.bin", "6");                                   // folder name in another case
    e.mod("Wrapper/Inner/romfs/f7.bin", "7");                             // zip with a wrapper folder
    e.mod("Dup/Dup/romfs/f8.bin", "8");                                   // the wrapper repeats the mod's name
    e.mod("Plain/romfs/f9.bin", "9");
    e.mod("Other/atmosphere/contents/0100AAAAAAAAAAAA/romfs/zz.bin", "z");   // another game's mod
    e.mod("Nothing/readme.txt", "hello");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    const std::set<std::string> want = {"AtmoA", "AtmoB", "atmosphere", "RyuMod", "EdenMod", "CaseMod", "Wrapper/Inner", "Dup", "Plain"};
    CHECK(mod_names(ov) == want);
    Built b(e, ov.get());
    for (int i = 1; i <= 9; i++) CHECK_EQ(b.r.get("/f" + std::to_string(i) + ".bin"), std::to_string(i));
    CHECK_EQ(b.r.get("/zz.bin"), std::string("<absent>"));
    CHECK_EQ(b.r.get("/base.bin"), std::string("base"));
    CHECK_EQ(cap.info({"skipping", "0100AAAAAAAAAAAA", "this game is 01006F8002326000"}), 1);
    CHECK_EQ(cap.warn({"folder 'Nothing' holds no RomFS content"}), 1);
    CHECK_EQ(cap.warn({"folder 'Other'"}), 0);
    same_tree(e, ov, b);
}

NXTEST(any_title_id_is_taken_when_the_title_is_unknown) {
    Env e;
    e.title = 0;
    e.mod("A/atmosphere/contents/0100AAAAAAAAAAAA/romfs/zz.bin", "z");
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(mod_names(ov).size(), 1u);
}

NXTEST(layouts_that_do_not_load_say_why) {
    Env e;
    e.mod("Good/romfs/g.bin", "g");
    e.mod("NoRomfs/Textures/t.bin", "t");        // files but no romfs folder
    std::error_code ec;
    stdfs::create_directories(e.mods / "Empty" / "romfs" / "emptydir", ec);   // a romfs folder with only an empty folder
    e.mod("Pack.zip", "PK");
    e.mod("romfs/loose.bin", "x");                // a romfs folder straight in mods/
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(mod_names(ov).count("Good"), 1u);
    CHECK_EQ(cap.warn({"folder 'NoRomfs' holds no RomFS content", "NoRomfs/romfs"}), 1);
    CHECK_EQ(cap.warn({"'Pack.zip' is an archive", "extract"}), 1);
    CHECK_EQ(cap.warn({"'romfs' sits directly in the mods folder"}), 1);
    CHECK_EQ(cap.warn({"mod 'Empty'", "holds no files"}), 1);
}

NXTEST(mod_names_longer_than_a_romfs_allows_are_skipped_with_one_warning) {
    Env e;
    std::string long_name;
    for (int i = 0; i < 100; i++) long_name += "\xE4\xB8\xAD";   // 300 bytes of UTF-8, 100 characters on disk
    e.mod("M/romfs/ok.bin", "ok");
    e.mod("M/romfs/" + long_name + ".bin", "long");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(info_of(ov, "M")->files, 1u);
    CHECK_EQ(cap.warn({"mod 'M'", "1 entry skipped"}), 1);
}

// ---- stubs -------------------------------------------------------------------------------------------------------------------------

NXTEST(stub_markers_hide_game_files_and_folders) {
    Env e;
    e.game("a.bin", "base-a");
    e.game("gone/x.bin", "x");
    e.game("gone/y.bin", "y");
    e.game("dir/z.bin", "z");
    e.game("keep.bin", "keep");
    e.mod("S/romfs/new.bin", "new");
    e.mod("S/romfs_ext/a.bin.stub", "");
    e.mod("S/romfs_ext/gone.stub", "");
    e.mod("S/romfs_ext/nothing.bin.stub", "");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(info_of(ov, "S")->hides, 2u);   // a.bin and gone/; the third marker matches nothing
    Built b(e, ov.get());
    CHECK_EQ(b.r.type("/a.bin"), 0);
    CHECK_EQ(b.r.type("/gone"), 0);
    CHECK_EQ(b.r.type("/gone/x.bin"), 0);
    CHECK_EQ(b.r.get("/keep.bin"), std::string("keep"));
    CHECK_EQ(b.r.get("/dir/z.bin"), std::string("z"));
    CHECK_EQ(b.r.get("/new.bin"), std::string("new"));
    Volume v(e.base_s(), false, true, 0);
    v.set_overlay(ov);
    uint32_t type = 9;
    CHECK_EQ(v.entry_type("/a.bin", &type), rc::kPathNotFound);
    CHECK_EQ(v.entry_type("/gone/x.bin", &type), rc::kPathNotFound);
    std::shared_ptr<VFile> f;
    CHECK_EQ(v.open_file("/a.bin", kModeRead, &f), rc::kPathNotFound);
    same_tree(e, ov, b);
    CHECK_EQ(cap.info({"mod 'S'", "hides 2 game entries"}), 1);
}

NXTEST(a_stub_hides_what_lower_priority_mods_add_but_not_higher_ones) {
    for (int stub_on_top = 0; stub_on_top < 2; stub_on_top++) {
        Env e;
        e.game("g.bin", "game");
        e.mod("L/romfs/lowmod.bin", "from-L");
        e.mod("L/romfs/other.bin", "other");
        e.mod("H/romfs_ext/lowmod.bin.stub", "");
        e.mod("H/romfs_ext/g.bin.stub", "");
        e.mod("H/romfs/h.bin", "h");
        e.set_ini(stub_on_top ? "[order]\nH = on\nL = on\n" : "[order]\nL = on\nH = on\n");
        auto ov = e.load();
        REQUIRE(ov);
        Built b(e, ov.get());
        CHECK_EQ(b.r.get("/g.bin"), std::string("<absent>"));    // hidden whatever the order: the game's file is the lowest layer
        CHECK_EQ(b.r.get("/other.bin"), std::string("other"));
        CHECK_EQ(b.r.get("/h.bin"), std::string("h"));
        // H on top: its marker removes L's file. L on top: L's file was added after the marker and stays.
        CHECK_EQ(b.r.get("/lowmod.bin"), std::string(stub_on_top ? "<absent>" : "from-L"));
        same_tree(e, ov, b);
    }
}

NXTEST(a_higher_mod_can_refill_a_hidden_folder) {
    Env e;
    e.game("gone/x.bin", "x");
    e.game("gone/y.bin", "y");
    e.mod("Hider/romfs_ext/gone.stub", "");
    e.mod("Refill/romfs/gone/fresh.bin", "fresh");
    e.set_ini("[order]\nRefill = on\nHider = on\n");
    auto ov = e.load();
    REQUIRE(ov);
    Built b(e, ov.get());
    std::map<std::string, bool> kids;
    CHECK(b.r.list("/gone", &kids));
    CHECK_EQ(kids.size(), 1u);
    CHECK_EQ(b.r.get("/gone/fresh.bin"), std::string("fresh"));
    same_tree(e, ov, b);
}

NXTEST(stub_with_wrong_case_warns_and_ips_patches_are_reported) {
    Env e;
    e.game("Sound/bgm.bin", "bgm");
    e.mod("S/romfs/x.bin", "x");
    e.mod("S/romfs_ext/sound/bgm.bin.stub", "");
    e.mod("S/romfs_ext/patched.bin.ips", "IPS");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(cap.warn({"mod 'S'", "hides nothing", "/Sound/bgm.bin", "case"}), 1);
    Built b(e, ov.get());
    CHECK_EQ(b.r.get("/Sound/bgm.bin"), std::string("bgm"));
    CHECK_EQ(cap.warn({"NOT supported", "'S'", "romfs_ext .ips"}), 1);
}

// ---- case, types ------------------------------------------------------------------------------------------------------------------

NXTEST(paths_are_case_sensitive_and_a_mismatch_is_reported) {
    Env e;
    e.game("Font/a.bin", "game-font");
    e.game("Name.TXT", "game-name");
    e.mod("M/romfs/font/a.bin", "mod-font");
    e.mod("M/romfs/name.txt", "mod-name");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    Built b(e, ov.get());
    // Horizon would serve both spellings as different files, and so does the image.
    CHECK_EQ(b.r.get("/Font/a.bin"), std::string("game-font"));
    CHECK_EQ(b.r.get("/font/a.bin"), std::string("mod-font"));
    CHECK_EQ(b.r.get("/Name.TXT"), std::string("game-name"));
    CHECK_EQ(b.r.get("/name.txt"), std::string("mod-name"));
    CHECK_EQ(info_of(ov, "M")->overrides, 0u);
    CHECK_EQ(cap.warn({"mod 'M'", "'/name.txt'", "'/Name.TXT'", "only in case"}), 1);
    CHECK_EQ(cap.warn({"mod 'M'", "'/font'", "'/Font'", "only in case"}), 1);
    same_tree(e, ov, b);
}

NXTEST(two_mods_that_differ_only_in_case_are_reported) {
    Env e;
    e.mod("A/romfs/Dir/q.bin", "A");
    e.mod("B/romfs/dir/q.bin", "B");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(cap.warn({"differ only in case"}), 1);
    Built b(e, ov.get());
    CHECK_EQ(b.r.get("/Dir/q.bin"), std::string("A"));
    CHECK_EQ(b.r.get("/dir/q.bin"), std::string("B"));
}

NXTEST(a_file_where_the_game_has_a_folder_is_ignored) {
    Env e;
    e.game("thing/inner.bin", "inner");
    e.game("solo.bin", "solo");
    e.mod("M/romfs/thing", "i am a file");
    e.mod("M/romfs/solo.bin/inside.bin", "i am under a file");
    e.mod("M/romfs/fine.bin", "fine");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(cap.warn({"mod 'M'", "'/thing'", "file in the mod but a folder in the game"}), 1);
    CHECK_EQ(cap.warn({"mod 'M'", "'/solo.bin'", "folder in the mod but a file in the game"}), 1);
    Built b(e, ov.get());
    CHECK_EQ(b.r.type("/thing"), 1);
    CHECK_EQ(b.r.get("/thing/inner.bin"), std::string("inner"));
    CHECK_EQ(b.r.get("/solo.bin"), std::string("solo"));
    CHECK_EQ(b.r.get("/fine.bin"), std::string("fine"));
    same_tree(e, ov, b);
    CHECK_EQ(info_of(ov, "M")->files, 1u);
}

// ---- code patches ------------------------------------------------------------------------------------------------------------------

NXTEST(code_patches_get_one_clear_warning_and_romfs_still_loads) {
    Env e;
    e.mod("P/romfs/p.bin", "p");
    e.mod("P/exefs/main", "code");
    e.mod("Q/exefs_patches/fix/1234.ips", "IPS");
    e.mod("Q/cheats/0123456789ABCDEF.txt", "[cheat]");
    e.mod("R/romfs/r.bin", "r");
    e.mod("exefs_patches/loose/5678.pchtxt", "@nsobid");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(cap.warn({"NOT supported", "'P'", "exefs", "'Q'", "exefs_patches", "cheats", "mods/exefs_patches"}), 1);
    CHECK_EQ(cap.warn({"NOT supported", "'R'"}), 0);
    CHECK_EQ(cap.warn({"NOT supported"}), 1);
    CHECK_EQ(cap.info({"mod 'Q'", "no RomFS content"}), 1);
    Built b(e, ov.get());
    CHECK_EQ(b.r.get("/p.bin"), std::string("p"));
    CHECK_EQ(b.r.get("/r.bin"), std::string("r"));
}

NXTEST(disabled_code_mods_do_not_trigger_the_warning) {
    Env e;
    e.mod("P/romfs/p.bin", "p");
    e.mod("Q/exefs/main", "code");
    e.set_ini("[order]\nP = on\nQ = off\n");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(cap.warn({"NOT supported"}), 0);
}

// ---- packed romfs.bin --------------------------------------------------------------------------------------------------------------

namespace {
// Writes a RomFS image of `src` (built by RomfsImage itself) to `dest`.
void dump_image(const stdfs::path& src, const stdfs::path& dest) {
    RomfsImage img;
    std::string err;
    REQUIRE(img.build(u8s(src), &err));
    std::vector<uint8_t> all(static_cast<size_t>(img.size()));
    REQUIRE(img.read(0, all.data(), all.size()));
    std::error_code ec;
    stdfs::create_directories(dest.parent_path(), ec);
    std::ofstream f(dest, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(all.data()), static_cast<std::streamsize>(all.size()));
}
}  // namespace

NXTEST(packed_romfs_bin_members_load_and_loose_files_win_over_them) {
    Env e;
    e.game("shared.bin", "base-shared");
    e.game("bin_only.bin", "base-bin-only");
    TempTree src;
    put(src.root, "bin_only.bin", "from-bin-only");
    put(src.root, "shared.bin", "bin-shared-version");
    put(src.root, "sub/deep.bin", std::string(5000, 'd') + "END");
    put(src.root, "sub/empty.bin", "");
    dump_image(src.root, e.mods / "Pk" / "ROMFS.BIN");
    e.mod("Pk/romfs/shared.bin", "loose-shared");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    const auto* m = info_of(ov, "Pk");
    REQUIRE(m != nullptr);
    CHECK_EQ(m->files, 4u);
    CHECK_EQ(m->overrides, 2u);
    CHECK_EQ(m->additions, 2u);
    Built b(e, ov.get());
    CHECK_EQ(b.r.get("/shared.bin"), std::string("loose-shared"));
    CHECK_EQ(b.r.get("/bin_only.bin"), std::string("from-bin-only"));
    CHECK_EQ(b.r.get("/sub/deep.bin"), std::string(5000, 'd') + "END");
    CHECK_EQ(b.r.type("/sub/empty.bin"), 2);
    same_tree(e, ov, b);
    // Partial and out-of-range reads of a member through the volume.
    Volume v(e.base_s(), false, true, 0);
    v.set_overlay(ov);
    std::shared_ptr<VFile> f;
    REQUIRE(v.open_file("/sub/deep.bin", kModeRead, &f) == 0);
    char buf[8] = {};
    uint64_t got = 0;
    CHECK_EQ(f->read(4998, buf, 8, &got), 0u);
    CHECK_EQ(got, 5u);   // "ddEND": stops at the member's end, not the image file's
    CHECK_EQ(std::string(buf, 5), std::string("ddEND"));
    CHECK_EQ(f->read(5003, buf, 8, &got), 0u);
    CHECK_EQ(got, 0u);
}

NXTEST(damaged_romfs_bin_is_reported_not_fatal) {
    Env e;
    e.mod("Good/romfs/g.bin", "g");
    e.mod("Garbage/romfs.bin", std::string(100, '\x7f'));
    e.mod("Tiny/romfs.bin", "x");
    TempTree src;
    put(src.root, "a.bin", std::string(4000, 'a'));
    dump_image(src.root, e.mods / "Cut" / "romfs.bin");
    {
        const std::string full = slurp(e.mods / "Cut" / "romfs.bin");
        std::ofstream f(e.mods / "Cut" / "romfs.bin", std::ios::binary | std::ios::trunc);
        f.write(full.data(), static_cast<std::streamsize>(full.size() / 2));   // the tables are at the end: gone
    }
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(cap.warn({"mod 'Garbage'", "romfs.bin", "not usable"}), 1);
    CHECK_EQ(cap.warn({"mod 'Tiny'", "not usable", "too small"}), 1);
    CHECK_EQ(cap.warn({"mod 'Cut'", "not usable", "outside the file"}), 1);
    Built b(e, ov.get());
    CHECK_EQ(b.r.get("/g.bin"), std::string("g"));
}

NXTEST(read_packed_romfs_lists_files_with_offsets) {
    TempTree src, out;
    put(src.root, "one.bin", "11111");
    put(src.root, "d1/d2/two.bin", "2222222");
    std::error_code ec;
    stdfs::create_directories(src.root / "emptydir", ec);
    dump_image(src.root, out.root / "x.bin");
    std::vector<PackedEntry> files;
    std::vector<std::string> dirs;
    size_t skipped = 9;
    std::string err;
    REQUIRE(read_packed_romfs((out.root / "x.bin").wstring(), &files, &dirs, &skipped, &err));
    CHECK_EQ(skipped, 0u);
    CHECK_EQ(files.size(), 2u);
    const std::string image = slurp(out.root / "x.bin");
    for (const PackedEntry& f : files) {
        const std::string data = image.substr(static_cast<size_t>(f.offset), static_cast<size_t>(f.size));
        CHECK_EQ(data, f.path == "/one.bin" ? std::string("11111") : std::string("2222222"));
    }
    std::set<std::string> d(dirs.begin(), dirs.end());
    CHECK(d.count("/d1") && d.count("/d1/d2") && d.count("/emptydir"));
}

// ---- the no-mods path --------------------------------------------------------------------------------------------------------------

NXTEST(no_mods_leaves_the_image_byte_for_byte_the_same) {
    Env e;
    for (int i = 0; i < 40; i++) e.game("dir" + std::to_string(i % 5) + "/f" + std::to_string(i) + ".bin", std::string(static_cast<size_t>(i * 13), 'a' + (i % 20)));
    e.game("top.bin", "top");
    RomfsImage plain;
    std::string err;
    REQUIRE(plain.build(e.base_s(), &err));
    std::vector<uint8_t> want(static_cast<size_t>(plain.size()));
    REQUIRE(plain.read(0, want.data(), want.size()));

    auto same_as_plain = [&](const std::shared_ptr<const ModOverlay>& ov, const char* what) {
        CHECK_MSG(ov == nullptr, what);
        RomfsImage img;
        CHECK_MSG(img.build(e.base_s(), &err, ov.get()), what);
        CHECK_MSG(img.size() == plain.size(), what);
        std::vector<uint8_t> got(static_cast<size_t>(img.size()));
        CHECK_MSG(img.read(0, got.data(), got.size()), what);
        CHECK_MSG(got == want, what);
    };
    Cap cap;
    {   // the mods folder is empty
        same_as_plain(e.load(), "empty mods folder");
    }
    {   // there is no mods folder at all, and nothing is logged above debug level
        Env f;
        std::error_code ec;
        stdfs::remove_all(f.mods, ec);
        const int before = cap.total();
        CHECK(f.load() == nullptr);
        CHECK_EQ(cap.total(), before);
    }
    {   // [mods] enabled = false, with a mod present
        e.mod("M/romfs/top.bin", "mod-top");
        e.enabled = false;
        same_as_plain(e.load(), "mods switched off");
        CHECK_EQ(cap.info({"switched off"}), 1);
        e.enabled = true;
    }
    {   // the only mod is off in mods.ini
        e.set_ini("[order]\nM = off\n");
        same_as_plain(e.load(), "only disabled mods");
    }
    {   // a mod with no files in it
        Env g;
        g.mod("Empty/romfs/.placeholder_dir/x", "");
        std::error_code ec;
        stdfs::remove(g.mods / "Empty" / "romfs" / ".placeholder_dir" / "x", ec);
        stdfs::remove(g.mods / "Empty" / "romfs" / ".placeholder_dir", ec);
        CHECK(g.load() == nullptr);
    }
    // And the volume with no overlay is the plain folder.
    Volume v(e.base_s(), false, true, 0);
    v.set_overlay(nullptr);
    std::map<std::string, std::string> vol, img;
    walk_vol(v, "/", &vol);
    Built b(e, nullptr);
    walk_img(b.r, "/", &img);
    CHECK(vol == img);
    CHECK_EQ(img["/top.bin"], std::string("F3:top"));
}

// ---- both views agree, a bigger tree ------------------------------------------------------------------------------------------------

NXTEST(thousands_of_files_merge_and_both_views_agree) {
    Env e;
    for (int i = 0; i < 2000; i++) e.game("d" + std::to_string(i % 20) + "/g" + std::to_string(i) + ".bin", "game" + std::to_string(i));
    for (int i = 0; i < 1000; i++) e.mod("Big/romfs/d" + std::to_string(i % 20) + "/g" + std::to_string(i) + ".bin", "mod" + std::to_string(i));
    for (int i = 0; i < 1000; i++) e.mod("Big/romfs/n" + std::to_string(i % 7) + "/new" + std::to_string(i) + ".bin", "new" + std::to_string(i));
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(ov->file_count(), 2000u);
    CHECK_EQ(info_of(ov, "Big")->overrides, 1000u);
    CHECK_EQ(info_of(ov, "Big")->additions, 1000u);
    Built b(e, ov.get());
    CHECK_EQ(b.img.stats().files, 3000u);
    CHECK_EQ(b.r.get("/d3/g3.bin"), std::string("mod3"));
    CHECK_EQ(b.r.get("/d13/g1013.bin"), std::string("game1013"));
    CHECK_EQ(b.r.get("/n6/new6.bin"), std::string("new6"));
    CHECK_EQ(cap.total() < 10 ? 1 : 0, 1);   // a handful of lines for 2000 files
    const auto tree = same_tree(e, ov, b);
    CHECK_EQ(tree.size(), static_cast<size_t>(3000 + 20 + 7));
}

NXTEST(everything_at_once_image_and_volume_agree) {
    Env e;
    e.game("a.bin", "A");
    e.game("keep/k.bin", "K");
    e.game("gone/g1.bin", "G1");
    e.game("gone/g2.bin", "G2");
    e.game("Font/f.bin", "F");
    TempTree src;
    put(src.root, "packed.bin", "packed");
    put(src.root, "keep/k.bin", "packed-k");
    dump_image(src.root, e.mods / "Packed" / "romfs.bin");
    e.mod("Loose/atmosphere/contents/" + kTid + "/romfs/a.bin", "loose-a");
    e.mod("Loose/atmosphere/contents/" + kTid + "/romfs/font/f.bin", "case-different");
    e.mod("Loose/romfs_ext/gone/g1.bin.stub", "");
    e.mod("Third/romfs/third/t.bin", "t");
    e.set_ini("[order]\nLoose = on\nPacked = on\nThird = on\n");
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    Built b(e, ov.get());
    const auto t = same_tree(e, ov, b);
    CHECK_EQ(t.at("/a.bin"), std::string("F7:loose-a"));
    CHECK_EQ(t.at("/keep/k.bin"), std::string("F8:packed-k"));
    CHECK_EQ(t.count("/gone/g1.bin"), 0u);
    CHECK_EQ(t.at("/gone/g2.bin"), std::string("F2:G2"));
    CHECK_EQ(t.at("/Font/f.bin"), std::string("F1:F"));
    CHECK_EQ(t.at("/font/f.bin"), std::string("F14:case-different"));
}

// ---- configuration -----------------------------------------------------------------------------------------------------------------

NXTEST(config_keys_mods_enabled_and_dir) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const stdfs::path exe_dir = stdfs::path(std::wstring(buf, n)).parent_path();
    nxrt::config_set("mods", "dir", "");
    nxrt::config_set("mods", "enabled", "true");
    ModsConfig c = mods_config_from_runtime();
    CHECK(c.enabled);
    CHECK(stdfs::path(c.mods_dir) == exe_dir / "mods");
    CHECK(stdfs::path(c.ini_path) == exe_dir / "mods.ini");
    CHECK_EQ(c.title_id, nxrt::services::program_id_now());

    nxrt::config_set("mods", "dir", "my/mods");   // relative: next to the exe
    c = mods_config_from_runtime();
    CHECK(stdfs::path(c.mods_dir) == (exe_dir / "my" / "mods").lexically_normal());
    TempTree t;
    nxrt::config_set("mods", "dir", u8s(t.root).c_str());   // absolute: as given
    c = mods_config_from_runtime();
    CHECK(stdfs::path(c.mods_dir) == t.root.lexically_normal());
    nxrt::config_set("mods", "enabled", "false");
    CHECK(!mods_config_from_runtime().enabled);
    nxrt::config_set("mods", "ini", "lists/my_mods.ini");   // the list can live elsewhere
    CHECK(stdfs::path(mods_config_from_runtime().ini_path) == exe_dir / "lists" / "my_mods.ini");
    nxrt::config_set("mods", "dir", "");
    nxrt::config_set("mods", "enabled", "true");
    nxrt::config_set("mods", "ini", "");
}

// ---- the hook for game modules -----------------------------------------------------------------------------------------------------

namespace {
struct HookGuard {
    explicit HookGuard(ModsIndexHook h) { set_mods_index_hook(std::move(h)); }
    ~HookGuard() { set_mods_index_hook(nullptr); }
};
std::vector<uint8_t> bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
}  // namespace

NXTEST(index_hook_sees_old_and_new_sizes_and_can_serve_generated_files) {
    Env e;
    e.game("a.bin", "base-a");                      // 6 bytes
    e.game("keep.bin", "keep");
    e.game("System/table.bin", "table-v1");
    e.mod("M/romfs/a.bin", "longer-a");             // 8 bytes: replaces a game file with another size
    e.mod("M/romfs/n.bin", "new");                  // 3 bytes: an addition
    e.mod("M/romfs_ext/keep.bin.stub", "");
    int called = 0;
    std::vector<ModFileChange> seen;
    std::vector<std::string> hidden_seen;
    std::string old_table, mod_view;
    bool absent_ok = true, hidden_read = true, bad_path = true, dir_path = true;
    HookGuard g([&](ModsIndexEdit& ed) {
        called++;
        seen = ed.changes();
        hidden_seen = ed.hidden();
        std::vector<uint8_t> d;
        if (ed.read_file("/System/table.bin", &d)) old_table.assign(d.begin(), d.end());
        if (ed.read_file("/a.bin", &d)) mod_view.assign(d.begin(), d.end());      // the mod's version, not the game's
        absent_ok = !ed.read_file("/nothing.bin", &d);
        hidden_read = ed.read_file("/keep.bin", &d);                              // hidden by the stub
        bad_path = ed.replace_file("no-slash", bytes("x"));
        dir_path = ed.replace_file("/System", bytes("x"));
        ed.replace_file("/System/table.bin", bytes(old_table + "+" + std::to_string(seen.size()) + "changes"));
        ed.replace_file("/generated/deep/new.bin", bytes("fresh"));
    });
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(called, 1);
    REQUIRE(seen.size() == 2);
    CHECK_EQ(seen[0].path, std::string("/a.bin"));
    CHECK(seen[0].replaces_game_file);
    CHECK_EQ(seen[0].old_size, 6u);
    CHECK_EQ(seen[0].new_size, 8u);
    CHECK_EQ(seen[0].mod, std::string("M"));
    CHECK_EQ(seen[1].path, std::string("/n.bin"));
    CHECK(!seen[1].replaces_game_file);
    CHECK_EQ(seen[1].old_size, 0u);
    CHECK_EQ(seen[1].new_size, 3u);
    REQUIRE(hidden_seen.size() == 1);
    CHECK_EQ(hidden_seen[0], std::string("/keep.bin"));
    CHECK_EQ(old_table, std::string("table-v1"));
    CHECK_EQ(mod_view, std::string("longer-a"));
    CHECK(absent_ok);
    CHECK(!hidden_read);
    CHECK(!bad_path);
    CHECK(!dir_path);
    CHECK_EQ(cap.info({"generated 2 files"}), 1);
    Built b(e, ov.get());
    CHECK_EQ(b.r.get("/System/table.bin"), std::string("table-v1+2changes"));
    CHECK_EQ(b.r.get("/generated/deep/new.bin"), std::string("fresh"));
    CHECK_EQ(b.r.get("/a.bin"), std::string("longer-a"));
    const auto t = same_tree(e, ov, b);
    CHECK_EQ(t.at("/generated/deep/new.bin"), std::string("F5:fresh"));
    // The generated file reads through the volume like any other, and has no times.
    Volume v(e.base_s(), false, true, 0);
    v.set_overlay(ov);
    FileTimes ft;
    ft.valid = true;
    CHECK_EQ(v.file_times("/System/table.bin", &ft), 0u);
    CHECK(!ft.valid);
    std::shared_ptr<VFile> f;
    CHECK_EQ(v.open_file("/System/table.bin", kModeWrite, &f), rc::kReadOnlyFileSystem);
}

NXTEST(index_hook_is_not_called_without_mods_and_a_failing_hook_is_survived) {
    int called = 0;
    {
        HookGuard g([&](ModsIndexEdit&) { called++; });
        Env none;   // no mods at all
        CHECK(none.load() == nullptr);
        Env off;    // the only mod is off
        off.mod("M/romfs/x.bin", "x");
        off.set_ini("[order]\nM = off\n");
        CHECK(off.load() == nullptr);
        Env disabled;
        disabled.mod("M/romfs/x.bin", "x");
        disabled.enabled = false;
        CHECK(disabled.load() == nullptr);
        CHECK_EQ(called, 0);
    }
    Env e;
    e.mod("M/romfs/x.bin", "x");
    HookGuard g([&](ModsIndexEdit&) { throw std::runtime_error("boom"); });
    Cap cap;
    auto ov = e.load();
    REQUIRE(ov);
    CHECK_EQ(cap.warn({"mod hook failed", "boom"}), 1);
    Built b(e, ov.get());
    CHECK_EQ(b.r.get("/x.bin"), std::string("x"));
}

// ---- through fsp-srv ---------------------------------------------------------------------------------------------------------------

NXTEST(fsp_serves_the_merged_tree_through_storage_and_file_system) {
    Env e;
    e.game("a.bin", std::string(10, 'a'));
    e.game("Worlds/b.pak", "bee");
    e.game("gone.bin", "g");
    e.mod("M/romfs/a.bin", "modded-a");
    e.mod("M/romfs/Worlds/c.pak", "see");
    e.mod("M/romfs_ext/gone.bin.stub", "");
    nxsvc::Env env;
    struct Reset {
        ~Reset() {
            nxrt::config_clear();
            nxrt::services::reset_all();
        }
    } reset;
    nxrt::config_set("fs", "romfs", e.base_s().c_str());
    nxrt::config_set("fs", "save_dir", u8s(e.t.root / "save").c_str());
    nxrt::config_set("mods", "dir", u8s(e.mods).c_str());
    nxrt::config_set("mods", "ini", u8s(e.ini).c_str());
    nxrt::services::reset_all();   // the mods are loaded at the first RomFS use

    nxsvc::Session s = nxsvc::connect("fsp-srv");
    REQUIRE(s.valid());
    CHECK(s.call(1, nxsvc::Args().with_pid().u64(0)).ok());

    // The RomFS image, byte for byte the one built from the same mods.
    auto ov = e.load();
    REQUIRE(ov);
    Built want(e, ov.get());
    nxsvc::Session st = s.sub(s.call(200));
    REQUIRE(st.valid());
    const int64_t total = st.call(4).get<int64_t>(0);
    CHECK_EQ(static_cast<uint64_t>(total), want.img.size());
    std::vector<uint8_t> got(static_cast<size_t>(total)), expect(static_cast<size_t>(total));
    CHECK(st.call(0, nxsvc::Args().i64(0).i64(total).buf_b(got.data(), got.size())).ok());
    CHECK(want.img.read(0, expect.data(), expect.size()));
    CHECK(got == expect);

    // The read-only file system: the same tree.
    nxsvc::Session fsys = s.sub(s.call(2));
    REQUIRE(fsys.valid());
    auto path_call = [&](uint32_t cmd, const char* p, nxsvc::Args extra = {}) {
        struct PathBuf {
            char b[0x301] = {};
        };
        auto pb = std::make_unique<PathBuf>();
        std::strncpy(pb->b, p, 0x300);
        extra.buf_x(pb->b, 0x301);
        return fsys.call(cmd, extra);
    };
    auto r = path_call(8, "/a.bin", nxsvc::Args().u32(1));
    REQUIRE(r.ok());
    nxsvc::Session file = fsys.sub(r);
    char buf[16] = {};
    r = file.call(0, nxsvc::Args().u32(0).pad(4).i64(0).i64(16).buf_b(buf, 16));
    CHECK(r.ok());
    CHECK_EQ(r.get<uint64_t>(0), 8u);
    CHECK_EQ(std::string(buf, 8), std::string("modded-a"));
    CHECK_EQ(path_call(7, "/Worlds/c.pak").get<uint32_t>(0), 1u);
    CHECK_EQ(path_call(7, "/gone.bin").result, 0x202u);   // hidden by the marker
    r = path_call(9, "/Worlds", nxsvc::Args().u32(3));
    REQUIRE(r.ok());
    nxsvc::Session dir = fsys.sub(r);
    CHECK_EQ(dir.call(1).get<int64_t>(0), 2);
}
