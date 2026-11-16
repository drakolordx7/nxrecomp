// SPDX-License-Identifier: GPL-3.0-or-later
// Display tab: window mode and size, frame rate limit, VSync, field of view. Extras tab: HDR, low latency, the frame rate counter, shader
// preparation and the rows the registered game adds. This page is the pattern for the others: build() describes the rows, the Form moves
// values between the rows and Settings.
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "game_info.h"
#include "input_label.h"
#include "nxrt/game.h"
#include "pages.h"

namespace nxl {

namespace {

using nxrt::play::Corner;
using nxrt::play::InKind;
using nxrt::play::InputId;
namespace extras = nxrt::extras;

struct Size {
    int w, h;
};
// Window sizes in the drop-down. The last entry of the list is "Custom".
const Size kSizes[] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};
constexpr int kSizeCount = static_cast<int>(sizeof kSizes / sizeof kSizes[0]);

const int kCaps[] = {0, 30, 60, 72, 90, 120, 144, 165, 240, 360};
constexpr int kCapCount = static_cast<int>(sizeof kCaps / sizeof kCaps[0]);

const char* const kModeNames[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
const char* const kModeKeys[] = {"windowed", "borderless", "exclusive"};

int size_index(int w, int h) {
    for (int i = 0; i < kSizeCount; ++i)
        if (kSizes[i].w == w && kSizes[i].h == h) return i;
    return kSizeCount;   // Custom
}

int cap_index(int fps) {
    for (int i = 0; i < kCapCount; ++i)
        if (kCaps[i] == fps) return i;
    return kCapCount;    // a value from the file that is not in the list
}

std::string fov_text(int v) {
    if (v == 0) return "Game default";
    return (v > 0 ? "+" : "-") + std::to_string(std::abs(v)) + "\xC2\xB0";   // degree sign
}

class DisplayPage : public Page {
public:
    const char* title() const override { return "Display"; }

    void build(Form& f) override {
        f.heading("Window");
        f.combo(
            "Window mode", {kModeNames[0], kModeNames[1], kModeNames[2]},
            [](const Settings& s) {
                for (int i = 0; i < 3; ++i)
                    if (s.display.window_mode == kModeKeys[i]) return i;
                return 0;
            },
            [](Settings& s, int i) { s.display.window_mode = kModeKeys[std::clamp(i, 0, 2)]; });

        std::vector<std::string> sizes;
        for (const Size& z : kSizes) sizes.push_back(std::to_string(z.w) + " x " + std::to_string(z.h));
        sizes.push_back("Custom");
        size_combo_ = f.combo(
            "Window size", sizes, [](const Settings& s) { return size_index(s.display.width, s.display.height); },
            [](Settings&, int) {},   // the width and height boxes below carry the value
            [this]() { size_chosen(); });
        width_ = f.number_pair(
            "Custom size", 320, 16384, [](const Settings& s) { return s.display.width; }, [](Settings& s, int v) { s.display.width = v; },
            [](const Settings& s) { return s.display.height; }, [](Settings& s, int v) { s.display.height = v; }, [this]() { size_typed(); }, "pixels");
        f.hint("The size of the game window. Fullscreen modes use your desktop resolution.");

        f.heading("Frame rate");
        cap_combo_ = f.combo(
            "Frame rate limit", cap_items(kCapCount, 0), [](const Settings& s) { return cap_index(s.display.fps_cap); },
            [this](Settings& s, int i) { s.display.fps_cap = i < kCapCount ? kCaps[i] : custom_cap_; });
        f.checkbox("VSync", [](const Settings& s) { return s.display.vsync; }, [](Settings& s, bool v) { s.display.vsync = v; });
        f.hint("With VSync on, the display's refresh rate caps the frame rate. With G-Sync or FreeSync, leave it off and set a limit.");
        f.checkbox("Faster GPU memory (uses more video memory)", [](const Settings& s) { return s.display.fast_gpu_memory; },
                   [](Settings& s, bool v) { s.display.fast_gpu_memory = v; });
        f.checkbox("Fewer GPU pipeline waits", [](const Settings& s) { return s.display.fewer_gpu_waits; },
                   [](Settings& s, bool v) { s.display.fewer_gpu_waits = v; });
        f.hint("Both raise the frame rate in the larger areas. Faster GPU memory keeps copies of the game's vertex data in video memory (up to 2 GiB; not on cards with less than 4 GiB). Both apply at the next start.");

        f.heading("Picture");
        f.slider(
            "Field of view", -30, 40, [](const Settings& s) { return static_cast<int>(std::lround(s.display.fov_offset)); },
            [](Settings& s, int v) { s.display.fov_offset = v; }, fov_text);
    }

    void load(const Settings& s) override {
        custom_cap_ = s.display.fps_cap;
        Page::load(s);
        // a frame rate from the file that is not in the list shows as its own last entry
        cap_combo_->set_items(cap_items(kCapCount, cap_index(s.display.fps_cap) == kCapCount ? s.display.fps_cap : -1));
    }

private:
    static std::vector<std::string> cap_items(int count, int custom) {
        std::vector<std::string> caps;
        for (int i = 0; i < count; ++i) caps.push_back(kCaps[i] == 0 ? "Unlimited" : std::to_string(kCaps[i]) + " fps");
        caps.push_back(custom > 0 ? std::to_string(custom) + " fps (from controls.ini)" : "Custom (from controls.ini)");
        return caps;
    }
    void size_chosen() {
        const int i = size_combo_->value();
        if (i < 0 || i >= kSizeCount) return;
        width_->set_value(kSizes[i].w);
        row_after(width_)->set_value(kSizes[i].h);
    }
    void size_typed() { size_combo_->set_value(size_index(width_->value(), row_after(width_)->value())); }

    Row *size_combo_ = nullptr, *width_ = nullptr, *cap_combo_ = nullptr;
    int custom_cap_ = 60;
};

class ExtrasPage : public Page {
public:
    const char* title() const override { return "Extras"; }

    void build(Form& f) override {
        f.heading("Output");
        f.combo(
            "HDR", {"Off", "Auto (when the display offers it)", "On"}, [](const Settings& s) { return static_cast<int>(s.extras.hdr.mode); },
            [](Settings& s, int i) { s.extras.hdr.mode = static_cast<extras::HdrMode>(std::clamp(i, 0, 2)); });
        f.combo(
            "Low latency", {"Off", "Auto (NVIDIA Reflex or AMD Anti-Lag 2)", "On", "Boost (Reflex)"},
            [](const Settings& s) { return static_cast<int>(s.extras.latency.mode); },
            [](Settings& s, int i) { s.extras.latency.mode = static_cast<extras::LatencyMode>(std::clamp(i, 0, 3)); });
        f.heading("Frame rate counter");
        f.hint("A small box in a corner of the game with the frame rate and frame time. Top left keeps clear of the game's own text; the bottom corners sit over the menu prompts and the title screen's copyright line. F1 in the game opens the same options.", true);
        f.checkbox("Show the counter when the game starts", [](const Settings& s) { return s.fps.show; }, [](Settings& s, bool v) { s.fps.show = v; });
        f.combo(
            "Corner", {"Top left", "Top right", "Bottom left", "Bottom right"}, [](const Settings& s) { return static_cast<int>(s.fps.corner); },
            [](Settings& s, int i) { s.fps.corner = static_cast<Corner>(std::clamp(i, 0, 3)); });
        f.combo(
            "Detail", {"Frame rate", "Frame rate and frame time", "Also the 1% low and the worst frame"},
            [](const Settings& s) { return s.fps.detail; }, [](Settings& s, int i) { s.fps.detail = std::clamp(i, 0, 2); });
        key_text_ = f.text_row("Hotkey in the game", "");
        key_button_ = f.button("Change...", 110);
        f.heading("Shaders");
        prep_hint_ = f.hint("Compiles the shaders now so the first hours of play do not stutter. Takes a few minutes.", true);
        prep_id_ = f.button("Prepare shaders", 150, true);
        prep_button_ = rows().back().get();
        rows_.build(f, nxrt::play::OptTab::Display);
        if (const GameLauncher* gl = nxrt::game::info().launcher; gl && gl->extras_rows) gl->extras_rows(f);
    }

    void load(const Settings& s) override {
        fps_key_ = s.fps.key;
        Page::load(s);
        rows_.load(s);
        key_text_->set_text(input_label(fps_key_));
    }

    void state_changed() override {
        if (!prep_button_) return;
        const bool offered = host()->shader_prepare_offered();
        prep_hint_->set_visible(offered);
        prep_button_->set_visible(offered);
        prep_button_->set_enabled(!host()->shader_prepare_busy());
    }

    void store(Settings* s) override {
        Page::store(s);
        s->fps.key = fps_key_;
    }

    bool command(int id) override {
        if (id == prep_id_) {
            host()->prepare_shaders_now();
            return true;
        }
        if (id != key_button_) return false;
        host()->capture_input("the frame rate counter", [this](bool got, InputId in) {
            if (!got) return;
            if (in.kind != InKind::Key) {
                host()->message("The counter hotkey has to be a keyboard key.");
                return;
            }
            fps_key_ = in;
            key_text_->set_text(input_label(fps_key_));
            host()->changed();
        });
        return true;
    }

private:
    Row* key_text_ = nullptr;
    int key_button_ = -1;
    InputId fps_key_;
    GameOptionRows rows_;
    Row* prep_hint_ = nullptr;
    Row* prep_button_ = nullptr;
    int prep_id_ = -1;
};

}  // namespace

std::unique_ptr<Page> make_display_page() { return std::make_unique<DisplayPage>(); }
std::unique_ptr<Page> make_extras_page() { return std::make_unique<ExtrasPage>(); }

}  // namespace nxl
