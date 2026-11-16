// SPDX-License-Identifier: GPL-3.0-or-later
// Controller tab: stick dead zones, look inversion and sensitivity, trigger point, rumble, motion controls, the face button labels, and the button layout (what each
// controller button presses on the Switch pad).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/input_source.h"
#include "pages.h"

namespace nxl {

namespace {

using nxrt::play::kPadDefault;
using nxrt::play::kPadSrcCount;
namespace hid = nxrt::hid;

struct Choice {
    const char* name;
    hid::NpadButtonBit bit;
};
const Choice kChoices[] = {
    {"A", hid::kBtnA},
    {"B", hid::kBtnB},
    {"X", hid::kBtnX},
    {"Y", hid::kBtnY},
    {"L", hid::kBtnL},
    {"R", hid::kBtnR},
    {"ZL", hid::kBtnZL},
    {"ZR", hid::kBtnZR},
    {"Plus", hid::kBtnPlus},
    {"Minus", hid::kBtnMinus},
    {"D-pad up", hid::kBtnUp},
    {"D-pad down", hid::kBtnDown},
    {"D-pad left", hid::kBtnLeft},
    {"D-pad right", hid::kBtnRight},
    {"L stick click", hid::kBtnStickL},
    {"R stick click", hid::kBtnStickR},
};
constexpr int kChoiceCount = static_cast<int>(sizeof kChoices / sizeof kChoices[0]);

// Combo items: 0 Default, 1 Nothing, 2.. the single buttons, then Custom (a value the file holds that no single button gives).
constexpr int kNothingItem = 1;
constexpr int kCustomItem = 2 + kChoiceCount;

const char* const kSourceLabels[kPadSrcCount] = {
    "A / Cross",   "B / Circle",     "X / Square",      "Y / Triangle",   "LB / L1",       "RB / R1",
    "LT / L2",     "RT / R2",        "Start / Menu",  "Back / View",  "L stick click", "R stick click",
    "D-pad up",    "D-pad down",     "D-pad left",      "D-pad right",
};

std::vector<std::string> layout_items() {
    std::vector<std::string> items = {"Default", "Nothing"};
    for (const Choice& c : kChoices) items.push_back(c.name);
    items.push_back("Custom (from controls.ini)");
    return items;
}

uint64_t value_of_item(int item) {
    if (item == 0) return kPadDefault;
    if (item == kNothingItem) return 0;
    return hid::btn(kChoices[item - 2].bit);
}

int item_of_value(uint64_t v) {
    for (int i = 0; i < kCustomItem; ++i)
        if (value_of_item(i) == v) return i;
    return kCustomItem;
}

int position_of(double v, int lo, int hi) {
    return std::clamp(static_cast<int>(std::lround(v * 100.0)), lo, hi);
}

std::string percent(int v) { return std::to_string(v) + "%"; }

class PadPage : public Page {
public:
    const char* title() const override { return "Controller"; }

    void build(Form& f) override {
        f.heading("Controller");
        f.columns(2);
        f.slider(
            "Left dead zone", 0, 50, [](const Settings& s) { return position_of(s.pad.stick_deadzone, 0, 50); },
            [this](Settings& s, int v) {
                if (v != deadzone_pos_) s.pad.stick_deadzone = v / 100.0;   // the slider does not show every digit: keep the file's value
            },
            percent);
        f.slider(
            "Right dead zone", 0, 50, [](const Settings& s) { return position_of(s.pad.right_deadzone, 0, 50); },
            [this](Settings& s, int v) {
                if (v != right_deadzone_pos_) s.pad.right_deadzone = v / 100.0;
            },
            percent);
        f.slider(
            "Trigger point", 5, 100, [](const Settings& s) { return position_of(s.pad.trigger_threshold, 5, 100); },
            [this](Settings& s, int v) {
                if (v != trigger_pos_) s.pad.trigger_threshold = v / 100.0;
            },
            percent);
        f.checkbox(
            "Invert look (right stick up looks down)", [](const Settings& s) { return s.pad.invert_y; }, [](Settings& s, bool v) { s.pad.invert_y = v; });
        f.slider(
            "Look sensitivity", 25, 300, [](const Settings& s) { return position_of(s.pad.look_sens, 25, 300); },
            [this](Settings& s, int v) {
                if (v != look_pos_) s.pad.look_sens = v / 100.0;
            },
            percent);
        f.checkbox("Rumble", [](const Settings& s) { return s.pad.rumble; }, [](Settings& s, bool v) { s.pad.rumble = v; });
        f.checkbox("Motion controls (gyro)", [](const Settings& s) { return s.pad.gyro; }, [](Settings& s, bool v) { s.pad.gyro = v; });
        f.slider(
            "Rumble strength", 0, 200, [](const Settings& s) { return position_of(s.pad.rumble_scale, 0, 200); },
            [this](Settings& s, int v) {
                if (v != rumble_pos_) s.pad.rumble_scale = v / 100.0;
            },
            percent);
        f.combo(
            "Face buttons", {"By letter (Xbox A = Switch A)", "By position"},
            [](const Settings& s) { return s.pad.label_abxy ? 0 : 1; }, [](Settings& s, int i) { s.pad.label_abxy = (i == 0); });

        f.columns(1);
        f.heading("Button layout");
        f.hint("What each controller button presses on the Switch pad. Default keeps the built-in layout.", true);
        f.columns(3);
        const std::vector<std::string> items = layout_items();
        for (int i = 0; i < kPadSrcCount; ++i) {
            combos_[i] = f.combo(
                kSourceLabels[i], items,
                [i](const Settings& s) { return item_of_value(s.pad.remap[i]); },
                [this, i](Settings& s, int item) { s.pad.remap[i] = item == kCustomItem ? custom_[i] : value_of_item(item); });
        }
        f.columns(2);
        reset_id_ = f.button("Reset layout", 150, true);
        find_id_ = f.button("Find a button...", 150, true);
        f.columns(1);
    }

    void load(const Settings& s) override {
        deadzone_pos_ = position_of(s.pad.stick_deadzone, 0, 50);
        right_deadzone_pos_ = position_of(s.pad.right_deadzone, 0, 50);
        trigger_pos_ = position_of(s.pad.trigger_threshold, 5, 100);
        look_pos_ = position_of(s.pad.look_sens, 25, 300);
        rumble_pos_ = position_of(s.pad.rumble_scale, 0, 200);
        for (int i = 0; i < kPadSrcCount; ++i) custom_[i] = s.pad.remap[i];
        Page::load(s);
    }

    bool command(int id) override {
        if (id == find_id_) {
            // The player presses the button on the pad; that row's drop-down is shown highlighted so it is clear which one it is.
            host()->capture_pad_button([this](int row) {
                if (row >= 0 && row < kPadSrcCount) combos_[row]->flash();
            });
            return true;
        }
        if (id != reset_id_) return false;
        for (Row* c : combos_) c->set_value(0);
        host()->changed();
        return true;
    }

private:
    int deadzone_pos_ = 10;
    int right_deadzone_pos_ = 10;
    int trigger_pos_ = 35;
    int look_pos_ = 100;
    int rumble_pos_ = 100;
    uint64_t custom_[kPadSrcCount] = {};   // the value of each button when the page opened; written back while Custom is selected
    Row* combos_[kPadSrcCount] = {};
    int reset_id_ = 0;
    int find_id_ = 0;
};

}  // namespace

std::unique_ptr<Page> make_pad_page() { return std::make_unique<PadPage>(); }

}  // namespace nxl
