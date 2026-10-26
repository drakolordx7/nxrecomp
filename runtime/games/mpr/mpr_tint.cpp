// SPDX-License-Identifier: GPL-3.0-or-later
// Suit and arm cannon colour (S45). See mpr_tint.h for what is recoloured and how; docs/S45.md for how the textures were found.
#include "mpr_tint.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/frame_limiter.h"
#include "nxrt/log.h"
#include "nxrt/services.h"
#include "nxrt/tex_edit.h"
#include "mpr_texedit.h"
#include "pak.h"
#include "txtr.h"

namespace nxrt::play {
namespace tint {
namespace {

constexpr const char* kTag = "tint";
constexpr uint64_t kDebounceNs = 250ull * 1000000ull;   // a change is applied once the colour has been still this long
constexpr uint64_t kPreviewNs = 300ull * 1000000ull;    // the F1 page's values count while it keeps sending them

// ---- the textures ------------------------------------------------------------------------------------------------------------
// Base colour (BCLR) textures of Samus, from the material slots of her models (docs/S45.md; tint_survey.py slots).
constexpr const char* kDup = "Preload/DuplicateData.pak";
constexpr const char* kGunPak = "Preload/MPR1/SamusGunMP1.pak";
constexpr const char* kAnimPak = "Preload/MPR1/TestAnimMP1.pak";
const Target kTargets[] = {
    {"26363018-a73b-497e-ac17-16346ec95552", kDup, Part::Suit, Profile::Warm, "Power Suit"},
    {"841522ae-3178-4e8e-862c-b03e59a66965", kDup, Part::Suit, Profile::Warm, "Varia Suit"},
    {"90110af2-a6ac-485f-bb0b-11368b1f76bb", kDup, Part::Suit, Profile::Gravity, "Gravity Suit"},
    {"ee146211-6658-4237-82c7-cd22cd9d9f40", kDup, Part::Suit, Profile::Phazon, "Phazon Suit"},
    {"f21a5ede-d2a3-4df3-b2bb-6bf30e9e914a", kDup, Part::Suit, Profile::Warm, "morph ball"},
    {"d1f4045d-a0e8-47af-86b2-b4e83c6465af", kDup, Part::Suit, Profile::Warm, "spider ball"},
    {"cf3b9d7c-a3a6-4eed-9cfa-58ad7697ce5f", kAnimPak, Part::Suit, Profile::Warm, "morph ball, low poly (Power)"},
    {"99e68612-d04d-4f4b-8fb1-bc2bbbf8126b", kAnimPak, Part::Suit, Profile::Warm, "morph ball, low poly (Varia)"},
    {"6d788bbf-ef6f-4ced-b7ef-a0f55bab1593", kAnimPak, Part::Suit, Profile::Gravity, "morph ball, low poly (Gravity)"},
    {"b55ecb6e-5d17-4e5f-ad2f-0b39484880b9", kAnimPak, Part::Suit, Profile::Phazon, "morph ball, low poly (Phazon)"},
    {"7e4e2cda-df78-4673-9acc-9a67581a7c6c", kGunPak, Part::Gun, Profile::Warm, "arm cannon (Power)"},
    {"e1341cb4-0fca-48de-9852-47f251c43b58", kGunPak, Part::Gun, Profile::Warm, "arm cannon (Varia)"},
    {"391a439d-0488-46b8-b56a-e8b1f79fb54c", kGunPak, Part::Gun, Profile::Gravity, "arm cannon (Gravity)"},
    {"38ee8213-3f64-4288-81c4-187eafbe48b9", kGunPak, Part::Gun, Profile::Phazon, "arm cannon (Phazon)"},
    {"29c99f56-0a18-43e5-9775-7bdf9fa070e9", kDup, Part::Gun, Profile::Teal, "beam gun (every suit and beam)"},
};
constexpr size_t kTargetCount = sizeof kTargets / sizeof kTargets[0];

// ---- colour math -------------------------------------------------------------------------------------------------------------
struct Lut {
    float to_lin[256];
    uint8_t to_srgb[4097];   // linear 0..1 in 4096 steps
    Lut() {
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            to_lin[i] = static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
        for (int i = 0; i <= 4096; ++i) {
            const double l = i / 4096.0;
            const double s = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
            to_srgb[i] = static_cast<uint8_t>(std::clamp(std::lround(s * 255.0), 0l, 255l));
        }
    }
};
const Lut& lut() {
    static const Lut l;
    return l;
}
inline uint8_t enc(float v) {
    v = std::clamp(v, 0.f, 1.f);
    return lut().to_srgb[static_cast<int>(v * 4096.f + 0.5f)];
}

inline float fast_cbrt(float x) {
    if (x <= 0.f) return 0.f;
    uint32_t i;
    std::memcpy(&i, &x, 4);
    i = i / 3u + 709921077u;
    float y;
    std::memcpy(&y, &i, 4);
    y = (2.f * y + x / (y * y)) * (1.f / 3.f);
    y = (2.f * y + x / (y * y)) * (1.f / 3.f);
    return y;
}

struct Lab {
    float L, a, b;
};
inline Lab oklab(float r, float g, float b) {
    const float l = fast_cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = fast_cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = fast_cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s, 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}
inline void oklab_to_lin(const Lab& c, float out[3]) {
    const float l_ = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    const float m_ = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    const float s_ = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;
    const float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    out[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    out[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    out[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

inline float smooth(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

struct HueWin {
    float centre, half;
};
struct ProfileDef {
    bool grey;
    HueWin win[2];
    int wins;
    float soft, sat0, sat1, val0, val1;
};
// Tuned on the previews (tint_preview.py, work/run_tint/preview). Warm: red, orange and beige paint (the Power and Varia suits, the ball),
// not the green, cyan and lime lights. Gravity adds the purple. Phazon: the light grey panels (its red lights are saturated and stay). Teal: the
// beam gun's grey-green.
const ProfileDef kProfiles[] = {
    {false, {{15.f, 40.f}, {0, 0}}, 1, 12.f, 0.12f, 0.25f, 0.04f, 0.12f},
    {false, {{15.f, 40.f}, {255.f, 30.f}}, 2, 12.f, 0.12f, 0.25f, 0.04f, 0.12f},
    {true, {{0, 0}, {0, 0}}, 0, 0.f, 0.10f, 0.20f, 0.30f, 0.45f},
    {false, {{160.f, 35.f}, {0, 0}}, 1, 15.f, 0.05f, 0.12f, 0.04f, 0.12f},
};

constexpr float kLMin = 0.24f, kLMax = 0.92f;

// ---- state ---------------------------------------------------------------------------------------------------------------------
struct Known {
    size_t gpu_size = 0;
    uint64_t hash = 0;
    uint32_t w = 0, h = 0, mips = 0, bw = 0, bh = 0;
};

struct Want {
    bool suit_on = false, gun_on = false;
    GlowRgb suit, gun;
    bool operator==(const Want& o) const {
        return suit_on == o.suit_on && gun_on == o.gun_on && (!suit_on || suit == o.suit) && (!gun_on || gun == o.gun);
    }
    bool operator!=(const Want& o) const { return !(*this == o); }
    bool any() const { return suit_on || gun_on; }
};

struct G {
    std::mutex mu;
    bool installed = false;
    bool hooks_set = false;
    // what the settings and the F1 page ask for
    Want saved, preview;
    uint64_t preview_ns = 0;
    Want pending;               // the last wanted value seen by tint_frame
    uint64_t pending_since = 0;
    Want applied;               // what the textures are being made with
    std::atomic<uint64_t> gen{1};
    // identity of the game's textures
    std::atomic<int> scan{0};   // 0 not started, 1 running, 2 done
    Known known[kTargetCount];
    std::map<uint64_t, PaintRef> refs;   // per target id: the paint reference of level 0
    Stats st;
};
G& g() {
    static G* s = new G();   // never destroyed: the texture hook may run on worker threads until the process ends
    return *s;
}

Want want_of(const TweakSettings& t) {
    Want w;
    w.suit_on = t.suit_color_on;
    w.suit = t.suit_color;
    w.gun_on = t.gun_color_on;
    w.gun = t.gun_color;
    return w;
}

// Reads the targets from the player's paks: size and hash of each tiled buffer. Worker thread; announces itself with a re-upload when done.
void scan_paks() {
    const std::string romfs = services::romfs_dir();
    std::map<std::string, std::unique_ptr<mprs::Pak>> paks;
    Known found[kTargetCount];
    uint32_t n = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kTargetCount; ++i) {
        const Target& tg = kTargets[i];
        try {
            auto it = paks.find(tg.pak);
            if (it == paks.end()) it = paks.emplace(tg.pak, mprs::Pak::open(mprs::open_file_storage((std::filesystem::path(romfs) / tg.pak).string()))).first;
            for (const mprs::PakEntry& e : it->second->assets()) {
                if (std::strcmp(e.type, "TXTR") != 0 || e.id.str() != tg.uuid) continue;
                const mprs::TxtrInfo info = mprs::txtr_read_info(*it->second, e);
                const mprs::TxtrFormat& f = mprs::txtr_format(info.format);
                if (!f.astc || info.layers != 1) break;
                const std::vector<mprs::u8> buf = mprs::txtr_read_tiled(*it->second, e, info);
                Known& k = found[i];
                k.gpu_size = buf.size();
                k.hash = hash(buf.data(), buf.size());
                k.w = info.width;
                k.h = info.height;
                k.mips = info.mips;
                k.bw = f.block_w;
                k.bh = f.block_h;
                ++n;
                break;
            }
        } catch (const std::exception& ex) {
            NXLOG_W(kTag, "%s (%s) in %s: %s", tg.what, tg.uuid, tg.pak, ex.what());
        }
    }
    {
        std::lock_guard<std::mutex> lk(g().mu);
        for (size_t i = 0; i < kTargetCount; ++i) g().known[i] = found[i];
        g().st.known = n;
    }
    g().scan.store(2, std::memory_order_release);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    NXLOG_I(kTag, "found %u of %zu suit and cannon textures in the game's paks (%.0f ms)", n, kTargetCount, ms);
    gfx::texture_edit_changed();   // textures uploaded before the table was ready
}

void start_scan() {
    int expect = 0;
    if (g().scan.compare_exchange_strong(expect, 1)) std::thread(&scan_paks).detach();
}

// ---- texture edit hooks ------------------------------------------------------------------------------------------------------
uint64_t hook_match(const gfx::TexEditReq& r, void*) {
    G& s = g();
    if (s.scan.load(std::memory_order_acquire) != 2) return 0;
    Want a;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        a = s.applied;
    }
    if (!a.any()) return 0;
    for (size_t i = 0; i < kTargetCount; ++i) {
        const Known& k = s.known[i];
        if (!k.gpu_size || k.w != r.width || k.h != r.height || k.mips != r.levels || k.bw != r.block_w || k.bh != r.block_h) continue;
        const bool on = kTargets[i].part == Part::Suit ? a.suit_on : a.gun_on;
        if (!on || r.tiled_size < k.gpu_size || hash(r.tiled, k.gpu_size) != k.hash) continue;
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.st.matches++ < 200) NXLOG_I(kTag, "%s (%.8s) recognised", kTargets[i].what, kTargets[i].uuid);
        return i + 1;
    }
    return 0;
}

uint64_t hook_generation(void*) { return g().gen.load(std::memory_order_acquire); }

void hook_edit(uint64_t id, uint32_t level, uint32_t w, uint32_t h, uint8_t* rgba, void*) {
    if (id == 0 || id > kTargetCount) return;
    G& s = g();
    const Target& tg = kTargets[id - 1];
    Want a;
    PaintRef ref;
    bool have_ref = false;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        a = s.applied;
        auto it = s.refs.find(id);
        if (it != s.refs.end()) {
            ref = it->second;
            have_ref = true;
        }
    }
    const bool on = tg.part == Part::Suit ? a.suit_on : a.gun_on;
    if (!on) return;
    if (!have_ref) {
        if (level != 0) return;   // the reference comes from level 0 (always edited first)
        ref = paint_ref(rgba, w, h, tg.profile);
        std::lock_guard<std::mutex> lk(s.mu);
        s.refs[id] = ref;
        NXLOG_I(kTag, "%s: paint reference L %.3f C %.3f", tg.what, ref.L, ref.C);
    }
    const GlowRgb want = tg.part == Part::Suit ? a.suit : a.gun;
    const size_t n = static_cast<size_t>(w) * h;
    // big levels in row bands on a few threads (the hook may run on the render thread)
    const unsigned hw = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
    if (n >= 256u * 256u && hw > 1) {
        std::vector<std::thread> th;
        const uint32_t band = (h + hw - 1) / hw;
        for (unsigned k = 0; k < hw; ++k) {
            const uint32_t y0 = k * band, y1 = std::min(h, y0 + band);
            if (y0 >= y1) break;
            th.emplace_back([=] { recolour(rgba + static_cast<size_t>(y0) * w * 4, static_cast<size_t>(y1 - y0) * w, tg.profile, ref, want); });
        }
        for (auto& t : th) t.join();
    } else {
        recolour(rgba, n, tg.profile, ref, want);
    }
    std::lock_guard<std::mutex> lk(s.mu);
    s.st.edits++;
}

void ensure_hooks() {
    G& s = g();
    if (s.hooks_set) return;
    s.hooks_set = true;
    static gfx::TexEditHooks h;
    h.match = &hook_match;
    h.generation = &hook_generation;
    h.edit = &hook_edit;
    h.user = nullptr;
    texedit_set(TexEditClient::Tint, &h);   // the hook is shared with the glow colour (mpr_texedit.h)
}

// Test aid: NXRT_TINT_TEST_FILE names a file of [tweaks] lines (suit_color = ..., gun_color = ...). When it changes, its values replace the
// saved ones. Checked twice a second.
void poll_test_file() {
    static const char* path = std::getenv("NXRT_TINT_TEST_FILE");
    if (!path || !*path) return;
    static int frames = 0;
    if (++frames % 30 != 0) return;
    static std::filesystem::file_time_type seen{};
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec || t == seen) return;
    seen = t;
    std::ifstream f(path);
    if (!f) return;
    TweakSettings tw;
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        bool known = false;
        std::string msg;
        if (!apply_tweaks_key(line.substr(0, eq), line.substr(eq + 1), &tw, &known, &msg)) NXLOG_W(kTag, "test file: %s", msg.c_str());
    }
    const gfx::TexEditStats es = gfx::texture_edit_stats();
    NXLOG_I(kTag, "test file: suit %s, gun %s (edits %llu, reuses %llu, last edit %.1f ms, last rebuild %.1f ms for %u textures)",
            tw.suit_color_on ? glow_rgb_text(tw.suit_color).c_str() : "off", tw.gun_color_on ? glow_rgb_text(tw.gun_color).c_str() : "off",
            static_cast<unsigned long long>(es.edits), static_cast<unsigned long long>(es.reuses), es.last_edit_ms, es.last_rebuild_ms,
            es.last_rebuild_textures);
    std::lock_guard<std::mutex> lk(g().mu);
    g().saved = want_of(tw);
}

}  // namespace

const Target* targets(size_t* count) {
    *count = kTargetCount;
    return kTargets;
}

uint64_t hash(const uint8_t* p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, p + i, 8);
        h ^= v * 0xC2B2AE3D27D4EB4Full;
        h = ((h << 31) | (h >> 33)) * 0x9E3779B185EBCA87ull;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return h;
}

float weight(uint8_t r8, uint8_t g8, uint8_t b8, Profile p) {
    const ProfileDef& d = kProfiles[static_cast<int>(p)];
    const float r = r8 / 255.f, gg = g8 / 255.f, b = b8 / 255.f;
    const float mx = std::max(r, std::max(gg, b)), mn = std::min(r, std::min(gg, b)), c = mx - mn;
    const float sat = mx > 0.f ? c / mx : 0.f;
    if (d.grey) return (1.f - smooth(d.sat0, d.sat1, sat)) * smooth(d.val0, d.val1, mx);
    if (c <= 0.f) return 0.f;
    float hp;
    if (mx == r) hp = std::fmod((gg - b) / c + 6.f, 6.f);
    else if (mx == gg) hp = (b - r) / c + 2.f;
    else hp = (r - gg) / c + 4.f;
    const float hue = hp * 60.f;
    float w = 0.f;
    for (int i = 0; i < d.wins; ++i) {
        float dd = std::fabs(std::fmod(hue - d.win[i].centre + 540.f, 360.f) - 180.f);
        w = std::max(w, 1.f - smooth(d.win[i].half, d.win[i].half + d.soft, dd));
    }
    return w * smooth(d.sat0, d.sat1, sat) * smooth(d.val0, d.val1, mx);
}

PaintRef paint_ref(const uint8_t* rgba, uint32_t w, uint32_t h, Profile p) {
    // weighted medians from histograms; every second texel in both directions is plenty
    constexpr int kBins = 512;
    constexpr float kCMax = 0.4f;
    std::vector<double> hl(kBins, 0.0), hc(kBins, 0.0);
    double tl = 0, tc = 0;
    const bool grey = kProfiles[static_cast<int>(p)].grey;
    const Lut& L = lut();
    for (uint32_t y = 0; y < h; y += (h > 1 ? 2 : 1)) {
        for (uint32_t x = 0; x < w; x += (w > 1 ? 2 : 1)) {
            const uint8_t* px = rgba + (static_cast<size_t>(y) * w + x) * 4;
            const float wt = weight(px[0], px[1], px[2], p);
            if (wt <= 0.f) continue;
            const Lab c = oklab(L.to_lin[px[0]], L.to_lin[px[1]], L.to_lin[px[2]]);
            const float C = std::sqrt(c.a * c.a + c.b * c.b);
            const double wl = grey ? wt : wt * C;
            hl[std::clamp(static_cast<int>(c.L * kBins), 0, kBins - 1)] += wl;
            hc[std::clamp(static_cast<int>(C / kCMax * kBins), 0, kBins - 1)] += wt;
            tl += wl;
            tc += wt;
        }
    }
    PaintRef ref;
    auto median = [](const std::vector<double>& hist, double total, float scale, float fallback) {
        if (total <= 0) return fallback;
        double acc = 0;
        for (int i = 0; i < kBins; ++i) {
            acc += hist[i];
            if (acc >= total / 2) return (i + 0.5f) / kBins * scale;
        }
        return fallback;
    };
    ref.L = std::clamp(median(hl, tl, 1.f, 0.5f), 0.05f, 0.95f);
    ref.C = std::max(median(hc, tc, kCMax, 0.05f), 0.02f);
    return ref;
}

void recolour(uint8_t* rgba, size_t n, Profile p, const PaintRef& ref, const GlowRgb& want) {
    const Lut& L = lut();
    const bool grey = kProfiles[static_cast<int>(p)].grey;
    const Lab t = oklab(L.to_lin[want.r], L.to_lin[want.g], L.to_lin[want.b]);
    const float Lt = std::clamp(t.L, kLMin, kLMax);
    const float lo = Lt / ref.L, hi = (1.f - Lt) / std::max(1.f - ref.L, 1e-3f);
    for (size_t i = 0; i < n; ++i) {
        uint8_t* px = rgba + i * 4;
        const float w = weight(px[0], px[1], px[2], p);
        if (w <= 0.f) continue;
        const float sr = L.to_lin[px[0]], sg = L.to_lin[px[1]], sb = L.to_lin[px[2]];
        const Lab c = oklab(sr, sg, sb);
        const float outL = c.L <= ref.L ? c.L * lo : Lt + (c.L - ref.L) * hi;
        const float k = grey ? 1.f : std::clamp(std::sqrt(c.a * c.a + c.b * c.b) / ref.C, 0.f, 1.25f);
        float o[3];
        oklab_to_lin(Lab{outL, t.a * k, t.b * k}, o);
        px[0] = enc(sr + (std::clamp(o[0], 0.f, 1.f) - sr) * w);
        px[1] = enc(sg + (std::clamp(o[1], 0.f, 1.f) - sg) * w);
        px[2] = enc(sb + (std::clamp(o[2], 0.f, 1.f) - sb) * w);
    }
}

Stats stats() {
    std::lock_guard<std::mutex> lk(g().mu);
    return g().st;
}

}  // namespace tint

using namespace tint;

void tint_install() {
    std::lock_guard<std::mutex> lk(g().mu);
    g().installed = true;
}

void tint_remove() {
    G& s = g();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.installed = false;
        s.applied = Want{};
        s.pending = Want{};
        s.saved = Want{};
    }
    if (s.hooks_set) {
        s.gen.fetch_add(1);
        gfx::texture_edit_changed();   // the edited textures go back to the game's
    }
}

void tint_set(const TweakSettings& t) {
    std::lock_guard<std::mutex> lk(g().mu);
    g().saved = want_of(t);
}

void tint_preview(const TweakSettings& t) {
    std::lock_guard<std::mutex> lk(g().mu);
    g().preview = want_of(t);
    g().preview_ns = FrameLimiter::now_ns();
}

void tint_frame() {
    G& s = g();
    poll_test_file();
    const uint64_t now = FrameLimiter::now_ns();
    bool apply = false;
    Want want;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.installed) return;
        want = (s.preview_ns && now - s.preview_ns < kPreviewNs) ? s.preview : s.saved;
        if (want != s.pending) {
            s.pending = want;
            s.pending_since = now;
        }
        if (s.pending != s.applied && now - s.pending_since >= kDebounceNs) {
            s.applied = s.pending;
            s.st.applies++;
            apply = true;
        }
    }
    if (!apply) return;
    NXLOG_I(kTag, "suit colour %s, arm cannon colour %s", want.suit_on ? glow_rgb_text(want.suit).c_str() : "off",
            want.gun_on ? glow_rgb_text(want.gun).c_str() : "off");
    if (want.any()) {
        start_scan();
        ensure_hooks();
    }
    s.gen.fetch_add(1, std::memory_order_acq_rel);
    if (s.hooks_set) gfx::texture_edit_changed();
}

}  // namespace nxrt::play
