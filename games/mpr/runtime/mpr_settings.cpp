// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's settings of controls.ini: the [tweaks] section (reader and writer) and the options the game adds to the base's
// sections, as one table (nxrt/game/settings.h GameOption): [aim] and [camera], the morph ball camera in [mouse] and [pad], and the post effects
// in [display]. The base reads, writes and shows the table (the F1 page and the launcher). Pure data: the launcher links it too.
#include "mpr_settings.h"

namespace nxrt::play {

namespace {

bool apply(GameState& st, const std::string& key, const std::string& value, bool* known, std::string* msg) {
    MprSettings& m = st.get<MprSettings>();
    bool mine = false;
    if (!fastboot_apply(key, value, &m.fastboot, &mine, msg)) return false;
    if (mine) {
        *known = true;
        return true;
    }
    return apply_tweaks_key(key, value, &m.tweaks, known, msg);
}

void format(const GameState& st, std::string* out) {
    const MprSettings& m = st.get<MprSettings>();
    fastboot_format(m.fastboot, out);
    *out += format_tweaks(m.tweaks);
}

const MprSettings& cm(const GameState& g) { return g.get<MprSettings>(); }
MprSettings& mm_(GameState& g) { return g.get<MprSettings>(); }

const char* const kLockWords[] = {"classic", "free"};
const char* const kLockLabels[] = {"Classic (the view turns to the target)", "Free (you keep control of the view)"};

// The morph ball camera's speed: 0.1 to 20 on the Source scale (the F1 slider's range); the keys in radians per count are the older form.
constexpr double kBallLo = 0.1, kBallHi = 20.0;

const GameOption kOptions[] = {
    // [mouse] the morph ball camera
    {.section = "mouse", .key = "ball_camera", .get = [](const GameState& g) { return cm(g).ball.on ? 1.0 : 0.0; },
     .set = [](GameState& g, double v) { mm_(g).ball.on = v != 0; }, .ui = kOptF1 | kOptLauncher, .tab = OptTab::Mouse,
     .label = "Mouse and right stick turn the morph ball camera"},
    {.section = "mouse", .key = "ball_sensitivity", .kind = OptKind::Real, .lo = kBallLo, .hi = kBallHi,
     .get = [](const GameState& g) { return source_sens(cm(g).ball.yaw_rad); },
     .set = [](GameState& g, double v) { mm_(g).ball.yaw_rad = mm_(g).ball.pitch_rad = rad_per_count(v); },   // both directions
     .ui = kOptF1 | kOptLauncher, .tab = OptTab::Mouse, .label = "Morph ball camera sensitivity", .depends = "ball_camera", .ui_lo = kBallLo, .ui_hi = kBallHi,
     .ui_decimals = 1, .ui_log = true},
    {.section = "mouse", .key = "morph_pitch", .kind = OptKind::Real, .lo = rad_per_count(kBallLo), .hi = rad_per_count(kBallHi),
     .get = [](const GameState& g) { return cm(g).ball.pitch_rad; }, .set = [](GameState& g, double v) { mm_(g).ball.pitch_rad = v; },
     .emit = [](const GameState& g) { return cm(g).ball.pitch_rad != cm(g).ball.yaw_rad; }},
    {.section = "mouse", .key = "morph_yaw", .kind = OptKind::Real, .lo = rad_per_count(kBallLo), .hi = rad_per_count(kBallHi),
     .get = [](const GameState& g) { return cm(g).ball.yaw_rad; }, .set = [](GameState& g, double v) { mm_(g).ball.yaw_rad = v; }, .write = false},
    // [pad]
    {.section = "pad", .key = "ball_orbit", .kind = OptKind::Real, .lo = 0.0, .hi = 6.0, .get = [](const GameState& g) { return cm(g).ball.orbit; },
     .set = [](GameState& g, double v) { mm_(g).ball.orbit = v; }},
    // [aim] and [camera]
    {.section = "aim", .key = "lock_mode", .kind = OptKind::Choice, .words = kLockWords, .word_count = 2,
     .get = [](const GameState& g) { return cm(g).aim.lock_mode == LockMode::Free ? 1.0 : 0.0; },
     .set = [](GameState& g, double v) { mm_(g).aim.lock_mode = v != 0 ? LockMode::Free : LockMode::Classic; }, .ui = kOptF1 | kOptLauncher,
     .tab = OptTab::Mouse, .group = "Lock-on and camera", .label = "Lock-on camera",
     .hint = "Free: the lock starts like the game's. Afterwards the view stays under your control (mouse and right stick) as long as the target stays on screen; "
             "the gun and the reticle stay on the target. Scanning always uses classic.",
     .labels = kLockLabels},
    {.section = "aim", .key = "free_lock_margin", .kind = OptKind::Real, .lo = 0.0, .hi = 0.4, .get = [](const GameState& g) { return cm(g).aim.free_lock_margin; },
     .set = [](GameState& g, double v) { mm_(g).aim.free_lock_margin = v; }, .ui = kOptF1 | kOptLauncher, .tab = OptTab::Mouse,
     .label = "Free lock edge margin", .depends = "lock_mode", .ui_lo = 0, .ui_hi = 40, .ui_scale = 100, .ui_unit = " %"},
    {.section = "camera", .key = "gun_drives_view", .alias = "view_sway", .get = [](const GameState& g) { return cm(g).camera.view_sway ? 1.0 : 0.0; },
     .set = [](GameState& g, double v) { mm_(g).camera.view_sway = v != 0; }, .ui = kOptF1 | kOptLauncher, .tab = OptTab::Mouse,
     .label = "Steady camera (walking bob and gun sway don't shake the view)",
     .hint = "On: the view follows only the mouse and the stick, and the idle drift, walking bob and landing dip move the gun on screen instead. Off (the game's own): "
             "the arm cannon is fixed to the view, so those movements move the whole view.",
     .inverted = true},
    // [display]
    {.section = "display", .key = "dynamic_resolution", .get = [](const GameState& g) { return cm(g).dynamic_resolution ? 1.0 : 0.0; },
     .set = [](GameState& g, double v) { mm_(g).dynamic_resolution = v != 0; }, .ui = kOptF1, .tab = OptTab::Display,
     .label = "Dynamic resolution (lowers the render size when the GPU is late)"},
    {.section = "display", .key = "bloom", .get = [](const GameState& g) { return cm(g).bloom ? 1.0 : 0.0; },
     .set = [](GameState& g, double v) { mm_(g).bloom = v != 0; }, .ui = kOptF1, .tab = OptTab::Display, .label = "Bloom"},
    {.section = "display", .key = "chromatic_aberration", .get = [](const GameState& g) { return cm(g).chromatic_aberration ? 1.0 : 0.0; },
     .set = [](GameState& g, double v) { mm_(g).chromatic_aberration = v != 0; }, .ui = kOptF1, .tab = OptTab::Display, .label = "Chromatic aberration"},
};

}

const GameSection& mpr_settings_section_data() {
    static const GameSection s = [] {
        GameSection g;
        g.name = "tweaks";
        g.apply = &apply;
        g.format = &format;
        g.options = kOptions;
        g.option_count = static_cast<unsigned>(sizeof kOptions / sizeof kOptions[0]);
        g.mouse_note = "Raw mouse input, no acceleration. Aim only applies in the first-person view when not locked on, not in the morph ball and not in a cutscene. "
                       "Holding the free look key (Alt) turns the view without turning Samus.";
        g.language_key = "language";
        return g;
    }();
    return s;
}

}
