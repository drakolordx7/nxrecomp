// SPDX-License-Identifier: GPL-3.0-or-later
// Button prompts (S17): see prompts.h.
//
// Pack file layout (scripts/build_prompt_pack.py), little endian: 24-byte header (magic "MPIP", version 1, count, width, height,
// reserved), `count` directory entries of 48 bytes (char name[40], u32 offset, u32 size) sorted by name, then the RGBA8 pixel blocks.
#ifdef _WIN32
#include <windows.h>
#endif

#include "prompts.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/services.h"
#include "nxrt/tex_override.h"
#include "pak.h"
#include "txtr.h"

namespace nxrt::play {

namespace {
constexpr const char* kTag = "prompts";
constexpr uint32_t kIconSize = 36;

uint32_t get_u32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

std::filesystem::path exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
#else
    return {};
#endif
}

bool read_file(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    out.clear();
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return f.gcount() == n;
}
}  // namespace

// ---- the icon pack ---------------------------------------------------------------------------------------------------------------
bool IconPack::load_memory(std::vector<uint8_t> bytes) {
    data_.clear();
    index_.clear();
    err_.clear();
    if (bytes.size() < 24 || std::memcmp(bytes.data(), "MPIP", 4) != 0) { err_ = "not an icon pack (bad magic)"; return false; }
    if (get_u32(bytes.data() + 4) != 1) { err_ = "unsupported icon pack version"; return false; }
    const uint32_t count = get_u32(bytes.data() + 8);
    w_ = get_u32(bytes.data() + 12);
    h_ = get_u32(bytes.data() + 16);
    const uint64_t px = static_cast<uint64_t>(w_) * h_ * 4;
    if (w_ == 0 || h_ == 0 || w_ > 256 || h_ > 256) { err_ = "bad icon size"; return false; }
    if (24 + static_cast<uint64_t>(count) * 48 > bytes.size()) { err_ = "directory is cut off"; return false; }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = bytes.data() + 24 + static_cast<size_t>(i) * 48;
        Entry en;
        en.name.assign(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 40));
        en.off = get_u32(e + 40);
        const uint32_t size = get_u32(e + 44);
        if (size != px || static_cast<uint64_t>(en.off) + size > bytes.size()) { err_ = "entry '" + en.name + "' is out of range"; return false; }
        index_.push_back(std::move(en));
    }
    std::sort(index_.begin(), index_.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    data_ = std::move(bytes);
    return true;
}

bool IconPack::load_file(const std::string& path) {
    std::vector<uint8_t> b;
    if (!read_file(path, b)) { err_ = "cannot read " + path; return false; }
    return load_memory(std::move(b));
}

const uint8_t* IconPack::find(const std::string& name) const {
    auto it = std::lower_bound(index_.begin(), index_.end(), name, [](const Entry& e, const std::string& n) { return e.name < n; });
    if (it == index_.end() || it->name != name) return nullptr;
    return data_.data() + it->off;
}

uint64_t prompt_hash(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

// ---- state -----------------------------------------------------------------------------------------------------------------------
namespace {

struct State {
    std::mutex mu;
    bool hook_set = false;
    // pack
    bool pack_tried = false;
    IconPack pack;
    // identity of the game's icon textures: (size of the tiled buffer, hash of it) -> icon
    bool ids_tried = false;
    std::map<std::pair<size_t, uint64_t>, Icon> ids;
    std::vector<size_t> id_sizes;
    // answers
    std::string names[kIconCount];
    bool any_name = false;
    PromptStyle effective = PromptStyle::Switch;
    // the inputs of the last update
    bool have_inputs = false;
    PromptInputs last_in;
    PromptStyle last_eff = PromptStyle::Switch;
    uint32_t hits = 0;
};

State& st() {
    static State s;
    return s;
}

void load_pack_locked(State& s) {
    if (s.pack_tried) return;
    s.pack_tried = true;
    std::vector<std::filesystem::path> cands;
    const std::string dir = config_get_str("prompts", "dir", "");
    if (!dir.empty()) cands.push_back(std::filesystem::path(dir) / "prompt_icons.pak");
    const std::filesystem::path ex = exe_dir();
    if (!ex.empty()) cands.push_back(ex / "prompts" / "prompt_icons.pak");
    std::error_code ec;
    cands.push_back(std::filesystem::current_path(ec) / "assets" / "prompts" / "prompt_icons.pak");
    if (!ex.empty()) cands.push_back(ex / "prompt_icons.pak");
    for (const auto& p : cands) {
        std::vector<uint8_t> b;
        if (!read_file(p, b)) continue;
        if (s.pack.load_memory(std::move(b))) {
            NXLOG_I(kTag, "icon pack %s: %zu icons of %ux%u", p.string().c_str(), s.pack.size(), s.pack.width(), s.pack.height());
            return;
        }
        NXLOG_W(kTag, "icon pack %s: %s", p.string().c_str(), s.pack.error().c_str());
    }
    NXLOG_W(kTag, "no icon pack (prompt_icons.pak) found: the game's own button icons stay");
}

// Reads the icon textures from the player's own paks and remembers a hash of each tiled buffer, so the hook can tell them from other textures.
void scan_ids_locked(State& s) {
    if (s.ids_tried) return;
    s.ids_tried = true;
    const std::string romfs = services::romfs_dir();
    std::map<std::string, std::unique_ptr<mprs::Pak>> paks;
    int found = 0;
    for (int i = 0; i < kIconCount; ++i) {
        const Icon ic = static_cast<Icon>(i);
        const std::string rel = icon_pak_file(ic);
        try {
            auto it = paks.find(rel);
            if (it == paks.end()) {
                const std::filesystem::path p = std::filesystem::path(romfs) / rel;
                it = paks.emplace(rel, mprs::Pak::open(mprs::open_file_storage(p.string()))).first;
            }
            const mprs::Pak& pak = *it->second;
            for (const mprs::PakEntry& e : pak.assets()) {
                if (std::strcmp(e.type, "TXTR") != 0 || e.id.str() != icon_asset_uuid(ic)) continue;
                const mprs::TxtrInfo info = mprs::txtr_read_info(pak, e);
                if (info.width != kIconSize || info.height != kIconSize || info.mips != 1) break;
                const std::vector<mprs::u8> buf = mprs::txtr_read_tiled(pak, e, info);
                s.ids[{buf.size(), prompt_hash(buf.data(), buf.size())}] = ic;
                if (std::find(s.id_sizes.begin(), s.id_sizes.end(), buf.size()) == s.id_sizes.end()) s.id_sizes.push_back(buf.size());
                ++found;
                break;
            }
        } catch (const std::exception& ex) {
            NXLOG_W(kTag, "%s in %s: %s", icon_asset_name(ic), rel.c_str(), ex.what());
        }
    }
    NXLOG_I(kTag, "recognised %d of %d icon textures in %s", found, kIconCount, romfs.c_str());
}

bool hook(const gfx::TexOverrideReq& r, uint8_t* rgba, void*) {
    if (r.width != kIconSize || r.height != kIconSize || !r.tiled) return false;
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.any_name) return false;
    scan_ids_locked(s);
    load_pack_locked(s);
    for (size_t n : s.id_sizes) {
        if (r.tiled_size < n) continue;
        auto it = s.ids.find({n, prompt_hash(r.tiled, n)});
        if (it == s.ids.end()) continue;
        const std::string& name = s.names[static_cast<int>(it->second)];
        if (name.empty()) return false;
        const uint8_t* px = s.pack.find(name);
        if (!px && name.compare(0, 4, "kbm/") == 0) px = s.pack.find("kbm/any");
        if (!px || s.pack.width() != kIconSize || s.pack.height() != kIconSize) return false;
        std::memcpy(rgba, px, static_cast<size_t>(kIconSize) * kIconSize * 4);
        if (s.hits++ < 40) NXLOG_I(kTag, "%s -> %s", icon_asset_name(it->second), name.c_str());
        return true;
    }
    return false;
}

}  // namespace

void prompts_update(const Settings& settings, const PromptInputs& in) {
    State& s = st();
    const PromptStyle eff = in.requested == PromptStyle::Auto ? auto_style(in.last, in.last_pad_kind, in.connected_pad_kind) : in.requested;
    bool changed = false, install = false;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        const bool same = s.have_inputs && s.last_eff == eff && s.last_in.label_abxy == in.label_abxy && s.last_in.ctx == in.ctx &&
                          s.last_in.settings_epoch == in.settings_epoch;
        if (same) return;
        s.have_inputs = true;
        s.last_in = in;
        s.last_eff = eff;
        ResolveIn r;
        r.style = eff;
        r.label_abxy = in.label_abxy;
        r.ctx = in.ctx;
        r.settings = &settings;
        bool any = false;
        for (int i = 0; i < kIconCount; ++i) {
            std::string n = resolve_icon(r, static_cast<Icon>(i));
            if (n != s.names[i]) changed = true;
            any |= !n.empty();
            s.names[i] = std::move(n);
        }
        if (eff != s.effective) {
            NXLOG_I(kTag, "button prompts: %s", prompt_style_name(eff));
            s.effective = eff;
        }
        s.any_name = any;
        if (any && !s.hook_set) {
            s.hook_set = true;
            install = true;
        }
    }
    // outside the lock: the hook takes it again on the render thread
    if (install) gfx::set_texture_override(&hook, nullptr);   // also asks for the icon textures to be uploaded again
    else if (changed) gfx::texture_override_changed();
}

PromptStyle prompts_effective_style() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.effective;
}

const char* prompts_effective_name() { return prompt_style_name(prompts_effective_style()); }

std::string prompts_icon_name(Icon i) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.names[static_cast<int>(i)];
}

void prompts_reset() {
    gfx::set_texture_override(nullptr, nullptr);
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.hook_set = false;
    s.pack_tried = s.ids_tried = false;
    s.pack = IconPack();
    s.ids.clear();
    s.id_sizes.clear();
    for (auto& n : s.names) n.clear();
    s.any_name = false;
    s.effective = PromptStyle::Switch;
    s.have_inputs = false;
    s.hits = 0;
}

}  // namespace nxrt::play
