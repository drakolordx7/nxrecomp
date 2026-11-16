// SPDX-License-Identifier: GPL-3.0-or-later
// Controls tab: mouse sensitivity (horizontal, vertical), mouse inversion and the wheel, the button icon style, and the rows of the options the
// registered game adds (nxrt/game/settings.h GameOption).
// The sensitivity sliders count tenths of the Source scale (1 = 0.1, 100 = 10.0). A slider only writes its value when the player
// moved it, so a value in the settings file with more digits than the slider can show stays as it is.
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "game_info.h"
#include "nxrt/game.h"
#include "pages.h"

namespace nxl {

namespace {

using nxrt::play::PromptStyle;
using nxrt::play::rad_per_count;
using nxrt::play::source_sens;

constexpr int kMin = 1;     // slider 1..100 = 0.1..10.0
constexpr int kMax = 100;

// The slider count of a stored value (Source scale times ten, rounded), before it is kept in range.
long steps_raw(double rad) {
    return std::lround(std::clamp(source_sens(rad) * 10.0, -1000.0, 1000.0));
}

int slider_pos(double rad) {
    return static_cast<int>(std::clamp(steps_raw(rad), static_cast<long>(kMin), static_cast<long>(kMax)));
}

std::string sens_text(int pos) {
    return std::to_string(pos / 10) + "." + std::to_string(pos % 10);
}

// The button icon drop-down, in the order it shows. The list index is what the page reads and writes; the enum value is what is saved.
const PromptStyle kStyles[] = {PromptStyle::Auto, PromptStyle::Keyboard, PromptStyle::Xbox, PromptStyle::PlayStation, PromptStyle::Switch};
constexpr int kStyleCount = static_cast<int>(sizeof kStyles / sizeof kStyles[0]);

bool same_steps(const Settings& s) {
    return steps_raw(s.mouse.yaw_rad) == steps_raw(s.mouse.pitch_rad);
}

class ControlsPage : public Page {
public:
    const char* title() const override { return "Controls"; }

    void build(Form& f) override {
        f.heading("Mouse");
        yaw_ = f.slider(
            "Horizontal sensitivity", kMin, kMax, [](const Settings& s) { return slider_pos(s.mouse.yaw_rad); },
            [this](Settings& s, int pos) {
                if (pos != yaw0_) s.mouse.yaw_rad = rad_per_count(pos / 10.0);
            },
            sens_text, [this]() { follow(yaw_, pitch_); });
        pitch_ = f.slider(
            "Vertical sensitivity", kMin, kMax, [](const Settings& s) { return slider_pos(s.mouse.pitch_rad); },
            [this](Settings& s, int pos) {
                if (pos != pitch0_) s.mouse.pitch_rad = rad_per_count(pos / 10.0);
            },
            sens_text, [this]() { follow(pitch_, yaw_); });
        f.checkbox(
            "Use the same sensitivity for both", [](const Settings& s) { return same_steps(s); },
            [](Settings&, bool) {},   // not saved: only the two sliders are
            [this]() { linked_ = !linked_; });
        f.checkbox(
            "Invert vertical aim", [](const Settings& s) { return s.mouse.invert_y; }, [](Settings& s, bool v) { s.mouse.invert_y = v; });
        f.checkbox(
            "Invert horizontal aim", [](const Settings& s) { return s.mouse.invert_x; }, [](Settings& s, bool v) { s.mouse.invert_x = v; });
        if (const nxrt::play::GameVocabulary* v = nxrt::play::game_vocabulary(); v && v->cycle) {
            f.checkbox(
                game_text(v->label_wheel_cycles, "Mouse wheel cycles"), [](const Settings& s) { return s.mouse.wheel_cycles; },
                [](Settings& s, bool v) { s.mouse.wheel_cycles = v; });
        }
        f.checkbox(
            "Mouse pointer in menus (cursor, hover, click)", [](const Settings& s) { return s.mouse.menu_cursor; },
            [](Settings& s, bool v) { s.mouse.menu_cursor = v; });
        rows_.build(f, nxrt::play::OptTab::Mouse);
        rows_.build(f, nxrt::play::OptTab::Pad);

        f.heading("Button prompts");
        f.combo(
            "Button icons", {"Automatic (follows what you use)", "Keyboard and mouse", "Xbox", "PlayStation", "Switch (the game's own)"},
            [](const Settings& s) {
                for (int i = 0; i < kStyleCount; ++i)
                    if (kStyles[i] == s.prompts.style) return i;
                return 0;
            },
            [](Settings& s, int i) { s.prompts.style = kStyles[std::clamp(i, 0, kStyleCount - 1)]; });
        f.hint("Automatic shows keyboard and mouse icons until you use a controller, then that controller's icons.");
        pad_text_ = f.text_row("Controller", "None detected");
    }

    void load(const Settings& s) override {
        yaw0_ = slider_pos(s.mouse.yaw_rad);
        pitch0_ = slider_pos(s.mouse.pitch_rad);
        linked_ = same_steps(s);
        Page::load(s);
        rows_.load(s);
        state_changed();
    }

    void state_changed() override {
        if (pad_text_ && host()) pad_text_->set_text(host()->controller_connected() ? "Connected" : "None detected");
    }

private:
    void follow(Row* from, Row* to) {
        if (!linked_) return;
        to->set_value(from->value());
    }

    Row *yaw_ = nullptr, *pitch_ = nullptr, *pad_text_ = nullptr;
    int yaw0_ = 0, pitch0_ = 0;
    GameOptionRows rows_;
    bool linked_ = false;                      // the "same for both" box (not saved)
};

}  // namespace

std::unique_ptr<Page> make_controls_page() { return std::make_unique<ControlsPage>(); }

}  // namespace nxl
