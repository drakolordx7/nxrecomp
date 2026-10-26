// SPDX-License-Identifier: GPL-3.0-or-later
// S44: glow colour tweak, see play_glow.h and docs/S44.md.
#include "play_glow.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/tex_edit.h"
#include "mpr_texedit.h"
#include "mpr_tint.h"
#include "play_aim.h"

namespace nxrt::play {

namespace {

constexpr const char* kTag = "glow";

// main-module relative addresses (Metroid Prime Remastered 1.0.0)
constexpr uint64_t kBallTables[] = {
    0x4b24a48,   // CMorphBallMP1::skBallNormalSwooshColors     (.bss, CColor4f[5], filled by the module's static init)
    0x4b24a98,   // CMorphBallMP1::skBallBoostedSwooshColors
    0x4b24ae8,   // CMorphBallMP1::skBallBoostedHullGlowColors
    0x4b24b38,   // CMorphBallMP1::skBallLightningSwooshColors
    0x4b24b88,   // CMorphBallMP1::skBallLightColors
};
constexpr int kTableCount = 5;
constexpr int kEntries = 5;                       // glow index: Power, Varia, Varia + Spider Ball, Gravity, Phazon
constexpr uint64_t kFnGunUpdate = 0xc93950;       // CPlayerGunMP1::Update(IMP1EntityBaseGOC&, float, float, float, CStateManager&): x0 the gun
constexpr uint64_t kPreviewNs = 250'000'000;      // the F1 page's preview holds this long after its last call

// A wanted colour: bit 24 set = recolour, low 24 bits = R G B.
constexpr uint32_t kOn = 1u << 24;
uint32_t pack(bool on, const GlowRgb& c) { return on ? (kOn | (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b) : 0u; }
GlowRgb unpack(uint32_t v) { return GlowRgb{uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; }

struct State {
    uint64_t base = 0;
    uint32_t hook = 0;
    std::atomic<uint32_t> ball{0}, gun{0}, suit{0};                // saved settings
    std::atomic<uint32_t> ball_prev{0}, gun_prev{0}, suit_prev{0}; // the F1 page's preview
    std::atomic<uint64_t> preview_ns{0};
    // ball tables, main thread only
    bool tables_known = false;
    float vanilla[kTableCount][kEntries][4] = {};
    uint32_t ball_applied = 0;                           // what the tables hold: 0 = the game's values
    std::atomic<uint64_t> gun_writes{0};                 // frames the charge effect was tinted
    // glow ramp textures (S44b), main thread only
    bool recolor_hook = false;                           // the texture edit client is set (from the first time a ramp part is on)
    uint32_t ramps_applied[2] = {};                      // what the gun's and the suit's ramps were last uploaded with
    uint64_t ramps_changed_ns = 0;                       // when texture_edit_changed was last called
    std::atomic<uint64_t> ramp_levels{0};                // texture levels recoloured (test log)
};
State g;

bool previewing() {
    const uint64_t t = g.preview_ns.load(std::memory_order_relaxed);
    return t && FrameLimiter::now_ns() - t < kPreviewNs;
}
uint32_t want_ball() { return previewing() ? g.ball_prev.load(std::memory_order_relaxed) : g.ball.load(std::memory_order_relaxed); }
uint32_t want_gun() { return previewing() ? g.gun_prev.load(std::memory_order_relaxed) : g.gun.load(std::memory_order_relaxed); }
uint32_t want_suit() { return previewing() ? g.suit_prev.load(std::memory_order_relaxed) : g.suit.load(std::memory_order_relaxed); }

// ---- particle modulation (the charge glow and the ball's inner glow are particle effects whose colours are in the effect assets) ----
// CParticleGen::SetModulationColor(CColor4i const&, bool children) is virtual (+184); CElementGen's passes it on to its children and its
// colour evaluator. The particles of both glows are near white, so a modulation colour gives them the setting's colour.
constexpr uint32_t kVtSetModulation = 184;
constexpr uint32_t kGunWeapon = 1272;              // CPlayerGunMP1: the current CGunWeaponMP1
// CGunWeaponMP1::DrawMuzzleFx draws, while the charge stage (+1520) is not 0, the CParticleGen at +1392 + 16 * stage (the charge glow at the
// muzzle; stage 0 draws the idle muzzle effect module at +1416 instead).
constexpr uint32_t kWeaponChargeStage = 1520;
constexpr uint32_t kWeaponChargeGens = 1392;
constexpr int kMaxChargeStage = 2;
constexpr uint32_t kPlayerMorphBall = 0x9d0;       // CPlayerMP1: CMorphBallMP1*
constexpr uint32_t kBallGlowIndex = 12;            // CMorphBallMP1: the glow index (0..4)
constexpr uint32_t kBallInnerGlowGens = 0x2100;    // CMorphBallMP1: CElementGen* per glow index (BallInnerGlow_*), drawn without a modulation colour
constexpr uint32_t kWhite = 0xffffffffu;

uint32_t rgba_bytes(uint32_t want) {
    if (!(want & kOn)) return kWhite;
    const GlowRgb c = unpack(want);
    return uint32_t(c.r) | (uint32_t(c.g) << 8) | (uint32_t(c.b) << 16) | 0xff000000u;
}

// Calls gen->SetModulationColor(rgba, true) on the guest, with the colour on the guest stack below the caller's frame. *c is restored.
bool set_modulation(::Ctx* c, uint64_t gen, uint32_t rgba) {
    uint64_t vt = 0, fn = 0;
    if (!gen || !guest_read(gen, &vt, sizeof vt) || !vt || !guest_read(vt + kVtSetModulation, &fn, sizeof fn) || !fn) return false;
    const uint64_t sp = (c->x[31] - 512) & ~uint64_t(15);
    if (!guest_write(sp, &rgba, sizeof rgba)) return false;
    const ::Ctx saved = *c;
    c->x[31] = sp;
    c->x[0] = gen;
    c->x[1] = sp;
    c->x[2] = 1;
    dispatch_call(c, fn);
    *c = saved;
    return true;
}

// A small memory of the generators we tinted, so that turning the setting off gives them white back once.
struct Tinted {
    uint64_t gen[4] = {};
    uint32_t rgba[4] = {};
};
Tinted g_gun_tint, g_ball_tint;

// Keeps every generator in `gens` at `rgba` (called once per frame while on: a generator may restart or spawn children). A generator that was
// tinted gets white once when the setting goes off. A generator that left the list is not touched again (it may have been freed; a new one
// starts white).
void keep_tint(::Ctx* c, Tinted* t, const uint64_t gens[4], uint32_t rgba) {
    for (int i = 0; i < 4; ++i) {
        const uint64_t gen = gens[i];
        if (!gen) {
            t->gen[i] = 0;
            t->rgba[i] = kWhite;
            continue;
        }
        const uint32_t prev = t->gen[i] == gen ? t->rgba[i] : kWhite;
        if (rgba != kWhite || prev != kWhite) set_modulation(c, gen, rgba);
        t->gen[i] = gen;
        t->rgba[i] = rgba;
    }
}

thread_local bool t_in_gun_hook = false;

int gun_update_hook(::Ctx* c, uint64_t addr, void*) {
    if (t_in_gun_hook) return 0;
    const uint64_t gun = c->x[0];
    // The game's own update first (it may start or replace the charge effect), then the colour.
    t_in_gun_hook = true;
    dispatch_call(c, addr);
    t_in_gun_hook = false;
    const uint32_t want = want_gun();
    uint64_t weapon = 0;
    uint64_t gens[4] = {};
    if (gun && guest_read(gun + kGunWeapon, &weapon, sizeof weapon) && weapon) {
        int32_t stage = 0;
        if (guest_read(weapon + kWeaponChargeStage, &stage, sizeof stage) && stage >= 1 && stage <= kMaxChargeStage)
            guest_read(weapon + kWeaponChargeGens + 16u * static_cast<uint32_t>(stage), &gens[stage - 1], sizeof gens[0]);
    }
    const uint32_t rgba = rgba_bytes(want);
    const bool on = rgba != kWhite;
    keep_tint(c, &g_gun_tint, gens, rgba);
    if (on && (gens[0] || gens[1])) g.gun_writes.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

// ---- model incandescence: not used since S44c ----
// S44 gave the morph ball's draws CModelFlagsMPT colour mode 2 (CModelFlagsMPT::ColorModulateIncan) through three hook sites on
// CModelDataMP1::Render / RenderGun (0xfa9694, 0xfa6c0c, 0xfa7dd0, still in recomp/games/mpr/main.sites, no longer registered). In the mode-2
// variant of the hull shader (02fe1ae2e1b12445) that colour is a constant emission over the whole hull, so the shell turned pink. The ball's
// glowing stripes are its inner glow (a particle effect, tinted above) and its light (skBallLightColors), which take the colour without it.

// ---- glow ramps (S44b): the colour of the arm cannon's glowing panels and of the suit's lights ----
// Those glows are not the incandescence colour that CModelFlagsMPT mode 2 modulates. The cannon's material shader (for example fragment shader
// 102960b5d01faefd, the barrel) computes the glow as ramp.rgb * mask * intensity: the mask is a grey texture of the model (the panels'
// stripes), the colour comes from a small sRGB "ramp" texture (128 x 16, ASTC 8x5) looked up at a second UV set, and the intensity is a
// scalar of the material (no colour constant anywhere). The suit's lights are its glow map (ICAN) times the material's ICNC colour, and mode 2
// would replace the suit's alpha-blended and additive draws. So the textures themselves are recoloured as the game uploads them, through the
// texture edit hook (nxrt/tex_edit.h, shared with the tint in mpr_texedit.h). Recolouring replaces the hue and keeps the game's brightness
// (glow_recolor_texel), so every beam's ramp takes the setting's colour.
constexpr GlowTexture kTextures[] = {
    // the arm cannon's lit panels: the colour ramp of its glow shaders (102960b5d01faefd, 64223e9717113568). Every beam uses it (the beams'
    // own effects, Wave arcs, Ice frost, Plasma heat, are other textures and particles and keep their colours)
    {128, 16, 8, 0xe6ef56252d1ac680ull, GlowTexPart::Gun, "arm cannon panel ramp"},
    // a second, orange ramp (128 x 128) that the same cannon shader 102960b5d01faefd samples on some of its parts
    {128, 128, 8, 0x095d5c0d4843b57full, GlowTexPart::Gun, "arm cannon orange ramp"},
    // The suits' glow maps (their ICAN textures: the lights), sampled by the suit body's shader 3065a1ce7022c16a (glow = ICAN.rgb * c5[2] + c5[3],
    // c5[2] = 1 for the suits) and the first-person arm's 8f2ff87c2b6f057d. Asset ids from the suit models' texture slots; the tails are the
    // hash of the asset's own tiled buffer (the game's storage has the same size) and were each seen in the game's log. S44b named the Varia
    // maps "Power Suit": Samus wears the Varia Suit on the Frigate. Not the 1024 x 1024 glow texture that shader 3065a1ce7022c16a also samples
    // (tail 315648d5a9d07890): the hangar's red and green signal lights share it.
    {2048, 2048, 12, 0x16f61b95d95992baull, GlowTexPart::Suit, "Power Suit glow map (4683a5d3)"},
    {2048, 2048, 12, 0x8dbb31ab811c3e94ull, GlowTexPart::Suit, "Varia Suit glow map (9758f9d7)"},
    {2048, 2048, 12, 0x84f3c71fe022906cull, GlowTexPart::Suit, "Gravity Suit glow map (6be16557)"},
    {2048, 2048, 12, 0xa05845d9b66304e6ull, GlowTexPart::Suit, "Phazon Suit glow map (d6086cda)"},
    {2048, 1024, 12, 0x80d74ae9fa702e8cull, GlowTexPart::Suit, "Power Suit arm glow map (41178f09)"},
    {2048, 1024, 12, 0x2d9b3b57fa799f2aull, GlowTexPart::Suit, "Varia Suit arm glow map (834be0bf)"},
    {2048, 1024, 12, 0x2b3fb8dba4c2fc05ull, GlowTexPart::Suit, "Gravity Suit arm glow map (211798a7)"},
    {2048, 1024, 12, 0x0e29b3f9e87c90d5ull, GlowTexPart::Suit, "Phazon Suit arm glow map (dfe04668)"},
    // Two small glow textures of the Gravity and Phazon Suit models. The game uploads them only while Samus wears that suit (the glow maps
    // above are in memory in the hangar, these are not), so a hangar log never showed them. Their tails are the assets' own, as for the maps.
    // The Gravity Suit's cyan stripes on top of the shoulders: one 8 x 8 texture that fills both the colour and the glow slot of its material.
    {8, 8, 4, 0x6b0345201e4c882bull, GlowTexPart::Suit, "Gravity Suit shoulder stripes (7f5c390c)"},
    // The Phazon Suit's red glow lines (shoulders, chest): a 128 x 4 colour ramp, sampled by fragment shader 8da4e38b4246a74f.
    {128, 4, 8, 0x5b14cbbac87c70bbull, GlowTexPart::Suit, "Phazon Suit glow ramp (b9513805)"},
};
constexpr size_t kTextureCount = sizeof kTextures / sizeof kTextures[0];

std::atomic<uint64_t> g_ramp_gen{1};

uint32_t want_part(GlowTexPart p) { return p == GlowTexPart::Gun ? want_gun() : want_suit(); }

// Discovery aid: NXRT_GLOW_LOG_TEXTURES=1 logs the tail of every sRGB ASTC texture once. NXRT_GLOW_LOG_TEXTURES=WxH,WxH,... also claims
// every texture of those sizes under a discovery id (kDiscoveryFlag | n; its edit changes nothing), so the runtime's "edited by the host
// (id ...)" line names the texture that has the logged tail; an NVN trace then says which texture a draw samples.
constexpr uint64_t kDiscoveryFlag = 1ull << 55;
constexpr uint64_t kRowBits = 8;

uint64_t ramp_match(const gfx::TexEditReq& r, void*) {
    static const std::string log_env = [] {
        const char* e = std::getenv("NXRT_GLOW_LOG_TEXTURES");
        return std::string(e && *e != '0' ? e : "");
    }();
    if (!log_env.empty() && r.srgb) {
        static std::mutex mu;
        static std::map<uint64_t, uint64_t> seen;   // tail -> discovery number
        const uint64_t tail = glow_texture_tail_hash(r.tiled, r.tiled_size, r.width, r.height, r.levels);
        char size[32];
        std::snprintf(size, sizeof size, "%ux%u", r.width, r.height);
        const bool claim = ("," + log_env + ",").find("," + std::string(size) + ",") != std::string::npos;
        std::lock_guard<std::mutex> lk(mu);
        auto it = seen.find(tail);
        if (it == seen.end()) {
            it = seen.emplace(tail, 1000 + seen.size()).first;
            NXLOG_I(kTag, "texture %s %u levels ASTC %ux%u sRGB, %zu bytes, tail %016llx%s, discovery id %llx", size, r.levels, r.block_w, r.block_h,
                    r.tiled_size, static_cast<unsigned long long>(tail), claim ? "" : " (not claimed)",
                    static_cast<unsigned long long>(kDiscoveryFlag | it->second));
        }
        if (claim) return kDiscoveryFlag | it->second;
    }
    if (!r.srgb || r.block_w != 8 || r.block_h != 5) return 0;
    uint64_t tail = 0;
    for (size_t i = 0; i < kTextureCount; ++i) {
        const GlowTexture& tx = kTextures[i];
        if (tx.w != r.width || tx.h != r.height || tx.levels != r.levels || !(want_part(tx.part) & kOn)) continue;
        if (!tail) tail = glow_texture_tail_hash(r.tiled, r.tiled_size, r.width, r.height, r.levels);
        if (tail == tx.tail) return glow_texture_id(i, r.tiled, r.tiled_size);
    }
    return 0;
}

uint64_t ramp_generation(void*) { return g_ramp_gen.load(std::memory_order_acquire); }

void ramp_edit(uint64_t id, uint32_t level, uint32_t w, uint32_t h, uint8_t* rgba, void*) {
    const int row = glow_texture_row(id);   // -1: a discovery id, changes nothing
    if (row < 0) return;
    const GlowTexture& tx = kTextures[row];
    const uint32_t want = want_part(tx.part);
    if (!(want & kOn)) return;
    const GlowRgb c = unpack(want);
    const size_t n = static_cast<size_t>(w) * h;
    // Test aid: NXRT_GLOW_TEST_FILL=1 paints every texel in the colour at full strength (shows whether a texture reaches the screen at all).
    static const bool fill = [] {
        const char* e = std::getenv("NXRT_GLOW_TEST_FILL");
        return e && *e == '1';
    }();
    size_t lit = 0;
    uint8_t peak = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* p = rgba + 4 * i;
        const uint8_t m = std::max({p[0], p[1], p[2]});
        if (m > 16) ++lit;
        peak = std::max(peak, m);
        if (fill) p[0] = p[1] = p[2] = 255;
        glow_recolor_texel(p, true, c);
    }
    g.ramp_levels.fetch_add(1, std::memory_order_relaxed);
    if (level == 0) NXLOG_I(kTag, "%s: recoloured (level 0: %zu of %zu texels lit, peak %u)", tx.what, lit, n, peak);
}

// Main thread, every frame: when the gun's or the suit's wanted colour changed, the textures are made again (at most every 100 ms while the F1
// sliders move).
void ramps_frame() {
    const uint32_t gun = want_gun(), suit = want_suit();
    if (gun == g.ramps_applied[0] && suit == g.ramps_applied[1]) return;
    const uint64_t now = FrameLimiter::now_ns();
    if (g.ramps_changed_ns && now - g.ramps_changed_ns < 100'000'000ull) return;
    g.ramps_changed_ns = now;
    g.ramps_applied[0] = gun;
    g.ramps_applied[1] = suit;
    g_ramp_gen.fetch_add(1, std::memory_order_acq_rel);
    if (!g.recolor_hook) {
        if (!((gun | suit) & kOn)) return;
        g.recolor_hook = true;
        gfx::TexEditHooks h;
        h.match = &ramp_match;
        h.generation = &ramp_generation;
        h.edit = &ramp_edit;
        texedit_set(TexEditClient::Glow, &h);
    }
    gfx::texture_edit_changed();
}

}  // namespace

void glow_player_tick(::Ctx* c, uint64_t player) {
    if (!g.base || !player) return;
    uint64_t ball = 0;
    uint64_t gens[4] = {};
    if (guest_read(player + kPlayerMorphBall, &ball, sizeof ball) && ball) {
        int32_t idx = -1;
        if (guest_read(ball + kBallGlowIndex, &idx, sizeof idx) && idx >= 0 && idx < kEntries) guest_read(ball + kBallInnerGlowGens + 8u * idx, &gens[0], sizeof gens[0]);
    }
    const uint32_t rgba = rgba_bytes(want_ball());
    keep_tint(c, &g_ball_tint, gens, rgba);
}

namespace {

// Reads the five tables once static init has filled them.
bool read_tables() {
    float t[kTableCount][kEntries][4];
    for (int i = 0; i < kTableCount; ++i)
        if (!guest_read(g.base + kBallTables[i], t[i], sizeof t[i])) return false;
    bool any = false;
    for (int i = 0; i < kTableCount; ++i)
        for (int e = 0; e < kEntries; ++e)
            for (int k = 0; k < 4; ++k) {
                if (!std::isfinite(t[i][e][k])) return false;
                if (t[i][e][k] != 0.f) any = true;
            }
    if (!any) return false;
    std::memcpy(g.vanilla, t, sizeof t);
    return true;
}

// Test aid (S44 live check): NXRT_GLOW_TEST_FILE names a file of [tweaks] lines (glow_color = ..., ball_glow = ..., gun_glow = ...). When the
// file changes, its values replace the saved ones, as if they had been saved from the F1 page. Checked twice a second; off without the variable.
void poll_test_file() {
    static const char* path = std::getenv("NXRT_GLOW_TEST_FILE");
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
    NXLOG_I(kTag, "test file: ball tables known %d, ours %d, gun colour writes so far %llu", g.tables_known ? 1 : 0, (g.ball_applied & kOn) ? 1 : 0,
            static_cast<unsigned long long>(g.gun_writes.load()));
    NXLOG_I(kTag, "ramp texture levels recoloured so far: %llu", static_cast<unsigned long long>(g.ramp_levels.load()));
    glow_set(tw);
    NXLOG_I(kTag, "test file applied: %s", format_tweaks(tw).c_str());
}

}  // namespace

const GlowTexture* glow_textures(size_t* count) {
    *count = kTextureCount;
    return kTextures;
}

uint64_t glow_texture_id(size_t row, const uint8_t* tiled, size_t size) {
    // The row in the low bits, a hash of the whole storage above them: the texture edit hook keeps edited data per id, and the first upload
    // of a suit glow map has the same tail but an empty level 0 (it decodes to garbage, 95 % of the texels lit), so the complete texture must
    // not get the data made from that one.
    const uint64_t content = tiled && size ? tint::hash(tiled, size) : 0;
    return (static_cast<uint64_t>(row) + 1) | ((content & ((1ull << (55 - kRowBits)) - 1)) << kRowBits);
}

int glow_texture_row(uint64_t id) {
    if (!id || (id >> 55)) return -1;   // none, or a discovery id
    const uint64_t row = (id & ((1ull << kRowBits) - 1)) - 1;
    return row < kTextureCount ? static_cast<int>(row) : -1;
}

float glow_srgb_to_linear(uint8_t c) {
    const float s = c / 255.f;
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

uint64_t glow_texture_tail_hash(const uint8_t* tiled, size_t size, uint32_t w, uint32_t h, uint32_t levels) {
    // The last quarter of the storage: the smaller mip levels. The game streams level 0 in after them (the first upload of a suit glow map has
    // an empty level 0), so a hash of the whole storage changes while the tail stays.
    if (!tiled || !size) return 0;
    size_t n = (size / 4) & ~static_cast<size_t>(15);
    if (size < 4096 || !n) n = size;
    uint64_t hsh = tint::hash(tiled + (size - n), n);
    const uint64_t meta[4] = {size, w, h, levels};
    for (uint64_t m : meta) hsh = (hsh ^ m) * 0x100000001B3ull;
    return hsh;
}

uint8_t glow_linear_to_srgb(float v) {
    v = std::clamp(v, 0.f, 1.f);
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(std::clamp(s, 0.f, 1.f) * 255.f));
}

void glow_recolor_texel(uint8_t rgba[4], bool srgb, const GlowRgb& want) {
    float in[4], out[4];
    for (int k = 0; k < 3; ++k) in[k] = srgb ? glow_srgb_to_linear(rgba[k]) : rgba[k] / 255.f;
    in[3] = 1.f;
    glow_recolor(in, want, out);
    for (int k = 0; k < 3; ++k) rgba[k] = srgb ? glow_linear_to_srgb(out[k]) : static_cast<uint8_t>(std::lround(std::clamp(out[k], 0.f, 1.f) * 255.f));
}

void glow_recolor(const float vanilla[4], const GlowRgb& want, float out[4]) {
    const float peak = std::max({vanilla[0], vanilla[1], vanilla[2], 0.f});
    out[0] = glow_srgb_to_linear(want.r) * peak;
    out[1] = glow_srgb_to_linear(want.g) * peak;
    out[2] = glow_srgb_to_linear(want.b) * peak;
    out[3] = vanilla[3];
}

void glow_install(uint64_t game_base) {
    glow_remove();
    g.base = game_base;
    g.hook = hook_add_entry(game_base + kFnGunUpdate, &gun_update_hook);
}

void glow_remove() {
    if (g.recolor_hook) {
        texedit_set(TexEditClient::Glow, nullptr);
        g_ramp_gen.fetch_add(1, std::memory_order_acq_rel);
        gfx::texture_edit_changed();   // the recoloured textures go back to the game's
    }
    g.recolor_hook = false;
    g.ramps_applied[0] = g.ramps_applied[1] = 0;
    if (g.hook) hook_remove(g.hook);
    g.hook = 0;
}

void glow_set(const TweakSettings& t) {
    GlowRgb c;
    const bool ball = glow_effective(t, t.ball, &c);
    g.ball.store(pack(ball, c), std::memory_order_relaxed);
    GlowRgb d;
    const bool gun = glow_effective(t, t.gun, &d);
    g.gun.store(pack(gun, d), std::memory_order_relaxed);
    GlowRgb e;
    const bool suit = glow_effective(t, t.suit, &e);
    g.suit.store(pack(suit, e), std::memory_order_relaxed);
    g.preview_ns.store(0, std::memory_order_relaxed);
}

void glow_preview(const TweakSettings& t) {
    GlowRgb c, d;
    const bool ball = glow_effective(t, t.ball, &c);
    const bool gun = glow_effective(t, t.gun, &d);
    g.ball_prev.store(pack(ball, c), std::memory_order_relaxed);
    g.gun_prev.store(pack(gun, d), std::memory_order_relaxed);
    GlowRgb e;
    const bool suit = glow_effective(t, t.suit, &e);
    g.suit_prev.store(pack(suit, e), std::memory_order_relaxed);
    g.preview_ns.store(FrameLimiter::now_ns(), std::memory_order_relaxed);
}

void glow_frame() {
    if (!g.base) return;
    poll_test_file();
    ramps_frame();
    const uint32_t want = want_ball();
    if (want == g.ball_applied) return;
    if (!g.tables_known) {
        if (!(want & kOn)) return;
        if (!read_tables()) return;   // static init has not run yet: try again next frame
        g.tables_known = true;
    }
    int failed = 0;
    for (int i = 0; i < kTableCount; ++i) {
        for (int e = 0; e < kEntries; ++e) {
            float t[4];
            if (want & kOn) glow_recolor(g.vanilla[i][e], unpack(want), t);
            else std::memcpy(t, g.vanilla[i][e], sizeof t);
            // one entry per write: guest_write takes at most 64 bytes
            if (!guest_write(g.base + kBallTables[i] + 16u * e, t, sizeof t)) ++failed;
        }
    }
    if (failed) NXLOG_W(kTag, "morph ball glow: %d of %d table entries could not be written", failed, kTableCount * kEntries);
    if ((want & kOn) != (g.ball_applied & kOn))
        NXLOG_I(kTag, "morph ball glow: %s", (want & kOn) ? "recoloured" : "the game's own");
    g.ball_applied = want;
}

GlowStats glow_stats() {
    GlowStats s;
    s.ball_tables_known = g.tables_known;
    s.ball_written = (g.ball_applied & kOn) != 0;
    s.gun_writes = g.gun_writes.load(std::memory_order_relaxed);
    s.ramp_levels = g.ramp_levels.load(std::memory_order_relaxed);
    return s;
}

}  // namespace nxrt::play
