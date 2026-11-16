// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pack file layout (scripts/build_prompt_pack.py), little endian: 24-byte header (magic "MPIP", version 1, count, width, height,
// reserved), `count` directory entries of 48 bytes (char name[40], u32 offset, u32 size) sorted by name, then the RGBA8 pixel blocks.
#include "prompts.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#include "nxrt/os/process.h"
#include "nxrt/services.h"
#include "nxrt/tex_upload.h"

namespace nxrt::play {

namespace {
constexpr const char* kTag = "prompts";

uint32_t get_u32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

std::filesystem::path exe_dir() { return std::filesystem::path(os::process::exe_dir()); }

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

namespace {

struct State {
    std::mutex mu;
    bool hook_set = false;
    int sub = 0;                     // the texture upload subscription (nxrt/tex_upload.h)
    bool pack_tried = false;
    IconPack pack;
    // identity of the game's icon textures: (size of the tiled buffer, hash of it) -> icon
    bool ids_tried = false;
    std::map<std::pair<size_t, uint64_t>, Icon> ids;
    std::vector<size_t> id_sizes;
    std::string names[kIconCount];
    bool any_name = false;
    PromptStyle effective = PromptStyle::Switch;
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

// Asks the game to read its icon textures from the player's own files and remembers a hash of each tiled buffer, so the hook can tell them
// from other textures (PromptGame::scan; an icon can have more than one copy).
void scan_ids_locked(State& s) {
    if (s.ids_tried) return;
    s.ids_tried = true;
    const PromptGame* g = game::info().prompts;
    if (!g || !g->scan || g->atlas) return;
    const std::string romfs = services::romfs_dir();
    const int found = g->scan(romfs, [](void* user, Icon ic, const uint8_t* buf, size_t size) {
        State& st = *static_cast<State*>(user);
        st.ids[{size, prompt_hash(buf, size)}] = ic;
        if (std::find(st.id_sizes.begin(), st.id_sizes.end(), size) == st.id_sizes.end()) st.id_sizes.push_back(size);
    }, &s);
    NXLOG_I(kTag, "recognised %d icon textures (%d icons, in-game and front end copies) in %s", found, kIconCount, romfs.c_str());
}


// ---- the atlas path (PromptGame::atlas): the icons are cells of one BC3 atlas whose alpha is the shape (the game tints it)

// Redraws a pack icon into a w x h cell as an alpha mask: the icon's body opaque, its letters and details transparent, the way the game's own
// glyphs are cut out (the game draws the alpha with its own tint). The icon is fitted into the cell, centred, bilinear.
void mask_for_cell(const uint8_t* px, uint32_t pw, uint32_t ph, uint32_t w, uint32_t h, std::vector<uint8_t>& a) {
    a.assign(static_cast<size_t>(w) * h, 0);
    // the ink levels of this icon: luminance of its opaque texels, the 10th and 90th percentile
    std::vector<float> lum;
    for (uint32_t i = 0; i < pw * ph; ++i) {
        const uint8_t* p = px + i * 4;
        if (p[3] > 160) lum.push_back((0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.f);
    }
    float lo = 0.f, hi = 1.f;
    if (lum.size() > 8) {
        std::sort(lum.begin(), lum.end());
        lo = lum[lum.size() / 10];
        hi = lum[lum.size() * 9 / 10];
        if (hi - lo < 0.15f) { lo = 0.f; hi = std::max(hi, 0.5f); }
    }
    const float scale = std::min(static_cast<float>(w) / pw, static_cast<float>(h) / ph) * 0.92f;
    const float ow = pw * scale, oh = ph * scale;
    const float x0 = (w - ow) * 0.5f, y0 = (h - oh) * 0.5f;
    auto sample = [&](float fx, float fy) {
        fx = std::clamp(fx - 0.5f, 0.f, pw - 1.f);
        fy = std::clamp(fy - 0.5f, 0.f, ph - 1.f);
        const uint32_t ix = static_cast<uint32_t>(fx), iy = static_cast<uint32_t>(fy);
        const uint32_t jx = std::min(ix + 1, pw - 1), jy = std::min(iy + 1, ph - 1);
        const float tx = fx - ix, ty = fy - iy;
        auto m = [&](uint32_t x, uint32_t y) {
            const uint8_t* p = px + (static_cast<size_t>(y) * pw + x) * 4;
            const float l = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.f;
            return (p[3] / 255.f) * std::clamp((l - lo) / (hi - lo), 0.f, 1.f);
        };
        return (m(ix, iy) * (1 - tx) + m(jx, iy) * tx) * (1 - ty) + (m(ix, jy) * (1 - tx) + m(jx, jy) * tx) * ty;
    };
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const float u = (x + 0.5f - x0) / scale, v = (y + 0.5f - y0) / scale;
            if (u < 0 || v < 0 || u >= pw || v >= ph) continue;
            a[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(std::clamp(sample(u, v), 0.f, 1.f) * 255.f + 0.5f);
        }
}

// BC3 block of white texels with the given alpha (16 values, row-major): BC4 alpha with 8 levels, both colour endpoints white.
void bc3_block_white(const uint8_t* alpha, uint8_t* out) {
    uint8_t a0 = 0, a1 = 255;
    for (int i = 0; i < 16; ++i) { a0 = std::max(a0, alpha[i]); a1 = std::min(a1, alpha[i]); }
    uint64_t bits = 0;
    if (a0 != a1) {
        int pal[8];
        pal[0] = a0;
        pal[1] = a1;
        for (int i = 2; i < 8; ++i) pal[i] = ((8 - i) * a0 + (i - 1) * a1) / 7;
        for (int i = 0; i < 16; ++i) {
            int best = 0, bd = 1 << 20;
            for (int k = 0; k < 8; ++k) {
                const int d = std::abs(pal[k] - alpha[i]);
                if (d < bd) { bd = d; best = k; }
            }
            bits |= static_cast<uint64_t>(best) << (3 * i);
        }
    }
    out[0] = a0;
    out[1] = a1;
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<uint8_t>(bits >> (8 * i));
    out[8] = out[9] = out[10] = out[11] = 0xff;     // colour endpoints: both white
    out[12] = out[13] = out[14] = out[15] = 0;
}

bool atlas_dims(const PromptAtlas* at, const gfx::TexUploadReq& r) {
    return at && r.bc == at->bc && r.width == at->width && r.height == at->height && r.tiled && r.tiled_size >= at->storage_size && !r.astc;
}

bool hook_patch(uint64_t id, uint32_t w, uint32_t h, uint8_t* blocks, void*) {
    const PromptGame* g = game::info().prompts;
    const PromptAtlas* at = g ? g->atlas : nullptr;
    if (!at || id != 1 || at->bc != 3 || w != at->width || h != at->height) return false;
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    load_pack_locked(s);
    const uint32_t bw = w / 4;
    std::vector<uint8_t> a;
    bool any = false;
    for (unsigned i = 0; i < at->cell_count; ++i) {
        const PromptAtlasCell& c = at->cells[i];
        const std::string& name = s.names[static_cast<int>(c.icon)];
        if (name.empty() || c.x % 4 || c.y % 4 || c.w % 4 || c.h % 4 || c.x + c.w > w || c.y + c.h > h) continue;
        const uint8_t* px = s.pack.find(name);
        if (!px && name.compare(0, 4, "kbm/") == 0) px = s.pack.find("kbm/any");
        if (!px) continue;
        mask_for_cell(px, s.pack.width(), s.pack.height(), c.w, c.h, a);
        for (uint32_t by = 0; by < c.h / 4; ++by)
            for (uint32_t bx = 0; bx < c.w / 4; ++bx) {
                uint8_t blk[16];
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) blk[y * 4 + x] = a[static_cast<size_t>(by * 4 + y) * c.w + bx * 4 + x];
                bc3_block_white(blk, blocks + (static_cast<size_t>(c.y / 4 + by) * bw + c.x / 4 + bx) * 16);
            }
        any = true;
        if (s.hits++ < 40) NXLOG_I(kTag, "%s -> %s (atlas cell %u,%u)", icon_asset_name(c.icon), name.c_str(), c.x, c.y);
    }
    return any;
}

// The upload hook (nxrt/tex_upload.h): match recognises an icon texture by a hash of its storage and answers with the icon's number + 1; edit writes the
// pack's picture for the icon's current name.
uint64_t hook_match(const gfx::TexUploadReq& r, void*) {
    const PromptGame* g = game::info().prompts;
    if (g && g->atlas) {
        if (!atlas_dims(g->atlas, r)) return 0;
        State& s = st();
        {
            std::lock_guard<std::mutex> lk(s.mu);
            if (!s.any_name) return 0;
        }
        const uint64_t h = prompt_hash(r.tiled, static_cast<size_t>(g->atlas->storage_size));
        if (h == g->atlas->hash) return 1;
        static bool logged = false;
        if (!logged) {
            logged = true;
            NXLOG_I(kTag, "a %ux%u texture of the atlas's format is not the game's icon atlas (storage %zu, hash %016llx)", r.width, r.height, r.tiled_size,
                    static_cast<unsigned long long>(h));
        }
        return 0;
    }
    if (!g || g->icon_size == 0) return 0;
    const uint32_t kIconSize = g->icon_size;
    if (r.astc || r.width != kIconSize || r.height != kIconSize || !r.tiled) return 0;
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.any_name) return 0;
    scan_ids_locked(s);
    for (size_t n : s.id_sizes) {
        if (r.tiled_size < n) continue;
        auto it = s.ids.find({n, prompt_hash(r.tiled, n)});
        if (it == s.ids.end()) continue;
        if (s.names[static_cast<int>(it->second)].empty()) return 0;
        return static_cast<uint64_t>(it->second) + 1;
    }
    return 0;
}

bool hook_edit(uint64_t id, uint32_t, uint32_t w, uint32_t h, uint8_t* rgba, void*) {
    const PromptGame* g = game::info().prompts;
    if (!g || g->icon_size == 0 || id == 0 || id > static_cast<uint64_t>(kIconCount)) return false;
    const uint32_t kIconSize = g->icon_size;
    if (w != kIconSize || h != kIconSize) return false;
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    const Icon ic = static_cast<Icon>(id - 1);
    const std::string& name = s.names[static_cast<int>(ic)];
    if (name.empty()) return false;
    load_pack_locked(s);
    const uint8_t* px = s.pack.find(name);
    if (!px && name.compare(0, 4, "kbm/") == 0) px = s.pack.find("kbm/any");
    if (!px || s.pack.width() != kIconSize || s.pack.height() != kIconSize) return false;
    // A game that stores its UI textures upside down (row 0 of the storage is the bottom row of the picture; the UI flips v when it draws) gets
    // the upright icon of the pack with its rows reversed.
    for (uint32_t y = 0; y < kIconSize; ++y)
        std::memcpy(rgba + static_cast<size_t>(y) * kIconSize * 4, px + static_cast<size_t>(g->upside_down ? kIconSize - 1 - y : y) * kIconSize * 4,
                    static_cast<size_t>(kIconSize) * 4);
    if (s.hits++ < 40) NXLOG_I(kTag, "%s -> %s", icon_asset_name(ic), name.c_str());
    return true;
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
    if (install) {
        gfx::TexUploadHooks h;
        h.match = &hook_match;
        h.edit = &hook_edit;
        if (const PromptGame* g = game::info().prompts; g && g->atlas) {
            h.patch_bc = &hook_patch;
            h.patch_w = g->atlas->width;
            h.patch_h = g->atlas->height;
        }
        const int sub = gfx::texture_upload_subscribe(h);   // also asks for the icon textures to be uploaded again
        std::lock_guard<std::mutex> lk(s.mu);
        s.sub = sub;
    } else if (changed) {
        gfx::texture_upload_changed();
    }
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
    State& s = st();
    int sub = 0;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        sub = s.sub;
        s.sub = 0;
    }
    gfx::texture_upload_unsubscribe(sub);
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
