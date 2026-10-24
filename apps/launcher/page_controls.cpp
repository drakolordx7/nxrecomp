// SPDX-License-Identifier: GPL-3.0-or-later
// Controls tab: mouse sensitivity (horizontal, vertical, morph ball), mouse inversion and the wheel, and the button icon style.
// The sensitivity sliders count tenths of the Source scale (1 = 0.1, 100 = 10.0). A slider only writes its value when the player
// moved it, so a value in the settings file with more digits than the slider can show stays as it is.
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pages.h"

namespace mprl {

namespace {

using nxrt::play::LockMode;
using nxrt::play::PromptStyle;
using nxrt::play::rad_per_count;
using nxrt::play::source_sens;

constexpr int kMin = 1;     // slider 1..100 = 0.1..10.0
constexpr int kMax = 100;

// The slider count of a stored value (Source scale times ten, rounded), before it is kept in range.
long steps_raw(double rad) {
    return std::lround(std::clamp(source_sens(rad) * 10.0, -1000.0, 1000.0));
}

// Slider position of a stored value, kept in 1..100.
int slider_pos(double rad) {
    return static_cast<int>(std::clamp(steps_raw(rad), static_cast<long>(kMin), static_cast<long>(kMax)));
}

// Slider position as text: 1 -> "0.1", 20 -> "2.0", 100 -> "10.0".
std::wstring sens_text(int pos) {
    return std::to_wstring(pos / 10) + L"." + std::to_wstring(pos % 10);
}

// The button icon drop-down, in the order it shows. The list index is what the page reads and writes; the enum value is what is saved.
const PromptStyle kStyles[] = {PromptStyle::Auto, PromptStyle::Keyboard, PromptStyle::Xbox, PromptStyle::PlayStation, PromptStyle::Switch};
constexpr int kStyleCount = static_cast<int>(sizeof kStyles / sizeof kStyles[0]);

// The free-lock edge margin as a percent slider: 0.15 -> 15, 0..40 only.
int margin_pos(double m) {
    return static_cast<int>(std::clamp(std::lround(m * 100.0), 0L, 40L));
}

// Margin slider position as text: 15 -> "15 %".
std::wstring margin_text(int pos) {
    return std::to_wstring(pos) + L" %";
}

// The two sensitivities have the same rounded value, so the "same for both" box starts ticked.
bool same_steps(const Settings& s) {
    return steps_raw(s.mouse.yaw_rad) == steps_raw(s.mouse.pitch_rad);
}

class ControlsPage : public Page {
public:
    const wchar_t* title() const override { return L"Controls"; }

    void build(Form& f) override {
        f.heading(L"Mouse");
        yaw_ = f.slider(
            L"Horizontal sensitivity", kMin, kMax, [](const Settings& s) { return slider_pos(s.mouse.yaw_rad); },
            [this](Settings& s, int pos) {
                if (pos != yaw0_) s.mouse.yaw_rad = rad_per_count(pos / 10.0);
            },
            sens_text, [this]() { follow(yaw_, pitch_); });
        pitch_ = f.slider(
            L"Vertical sensitivity", kMin, kMax, [](const Settings& s) { return slider_pos(s.mouse.pitch_rad); },
            [this](Settings& s, int pos) {
                if (pos != pitch0_) s.mouse.pitch_rad = rad_per_count(pos / 10.0);
            },
            sens_text, [this]() { follow(pitch_, yaw_); });
        f.checkbox(
            L"Use the same sensitivity for both", [](const Settings& s) { return same_steps(s); },
            [](Settings&, bool) {},   // not saved: only the two sliders are
            [this]() { linked_ = !linked_; });
        f.slider(
            L"Morph ball camera", kMin, kMax, [](const Settings& s) { return slider_pos(s.mouse.morph_yaw_rad); },
            [this](Settings& s, int pos) {
                if (pos == morph0_) return;
                s.mouse.morph_yaw_rad = rad_per_count(pos / 10.0);
                s.mouse.morph_pitch_rad = s.mouse.morph_yaw_rad;
            },
            sens_text);
        f.checkbox(
            L"Invert vertical aim", [](const Settings& s) { return s.mouse.invert_y; },
            [](Settings& s, bool v) { s.mouse.invert_y = v; });
        f.checkbox(
            L"Invert horizontal aim", [](const Settings& s) { return s.mouse.invert_x; },
            [](Settings& s, bool v) { s.mouse.invert_x = v; });
        f.checkbox(
            L"Mouse wheel changes the beam", [](const Settings& s) { return s.mouse.wheel_beams; },
            [](Settings& s, bool v) { s.mouse.wheel_beams = v; });
        f.checkbox(
            L"Mouse pointer in menus (cursor, hover, click)", [](const Settings& s) { return s.mouse.menu_cursor; },
            [](Settings& s, bool v) { s.mouse.menu_cursor = v; });

        f.heading(L"Lock-on and camera");
        f.combo(
            L"Lock-on camera", {L"Classic (the view turns to the target)", L"Free (you keep control of the view)"},
            [](const Settings& s) { return s.aim.lock_mode == LockMode::Free ? 1 : 0; },
            [](Settings& s, int i) { s.aim.lock_mode = i == 1 ? LockMode::Free : LockMode::Classic; });
        f.hint(L"Free keeps the game's lock, homing shots and strafing but does not turn the view. Scanning always uses classic.", 2);
        f.slider(
            L"Free lock edge margin", 0, 40, [](const Settings& s) { return margin_pos(s.aim.free_lock_margin); },
            [this](Settings& s, int pct) {
                if (pct == margin0_) return;   // only a value the player moved is written
                s.aim.free_lock_margin = pct / 100.0;
            },
            margin_text);
        f.checkbox(
            L"Idle view sway (the view drifts while standing still)", [](const Settings& s) { return s.camera.idle_sway; },
            [](Settings& s, bool v) { s.camera.idle_sway = v; });
        f.checkbox(
            L"Walking view bob", [](const Settings& s) { return s.camera.walk_bob; },
            [](Settings& s, bool v) { s.camera.walk_bob = v; });

        f.heading(L"Button prompts");
        f.combo(
            L"Button icons", {L"Automatic (follows what you use)", L"Keyboard and mouse", L"Xbox", L"PlayStation", L"Switch (the game's own)"},
            [](const Settings& s) {
                for (int i = 0; i < kStyleCount; ++i)
                    if (kStyles[i] == s.prompts.style) return i;
                return 0;
            },
            [](Settings& s, int i) { s.prompts.style = kStyles[std::clamp(i, 0, kStyleCount - 1)]; });
        f.hint(L"Automatic shows keyboard and mouse icons until you use a controller, then that controller's icons.", 2);
        f.text_row(L"Controller", host()->controller_connected() ? L"Connected" : L"None detected");
    }

    void load(const Settings& s) override {
        yaw0_ = slider_pos(s.mouse.yaw_rad);
        pitch0_ = slider_pos(s.mouse.pitch_rad);
        morph0_ = slider_pos(s.mouse.morph_yaw_rad);
        margin0_ = margin_pos(s.aim.free_lock_margin);
        linked_ = same_steps(s);
        Page::load(s);
    }

private:
    // Linked sensitivity: the other slider takes the position of the one the player moved.
    void follow(HWND from, HWND to) {
        if (!linked_ || syncing_) return;
        syncing_ = true;
        SendMessageW(to, TBM_SETPOS, TRUE, SendMessageW(from, TBM_GETPOS, 0, 0));
        // The page handles this scroll message the same way as a drag, so the value text of `to` is redrawn.
        SendMessageW(hwnd(), WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, 0), reinterpret_cast<LPARAM>(to));
        syncing_ = false;
    }

    HWND yaw_ = nullptr, pitch_ = nullptr;
    int yaw0_ = 0, pitch0_ = 0, morph0_ = 0, margin0_ = 0;   // slider positions as loaded
    bool linked_ = false;                      // the "same for both" box (not saved)
    bool syncing_ = false;
};

}  // namespace

std::unique_ptr<Page> make_controls_page() { return std::make_unique<ControlsPage>(); }

}  // namespace mprl
