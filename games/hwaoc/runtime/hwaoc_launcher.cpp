// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's registration of Age of Calamity: the data module plus its Resolution tab (the base's neutral page API,
// nxrt/game/launcher_ui.h). The tab edits the base's [display] keys the game's display hooks read (hwaoc_display.h): render scale, a fixed render
// size and a forced aspect. The game's own [display] options (HUD, dynamic resolution) are rows of the base's Display tab (hwaoc_settings.cpp).
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "hwaoc_game.h"
#include "nxrt/game.h"
#include "nxrt/game/launcher_ui.h"

namespace nxl {

namespace {

constexpr double kAspects[] = {0.0, 16.0 / 9.0, 16.0 / 10.0, 21.0 / 9.0, 32.0 / 9.0};
const char* const kAspectNames[] = {"Follow the window", "16:9", "16:10", "21:9", "32:9"};
constexpr int kAspectCount = 5;

int aspect_index(double a) {
    for (int i = 1; i < kAspectCount; ++i)
        if (std::fabs(a - kAspects[i]) < 0.005) return i;
    return 0;
}

class ResolutionPage : public Page {
public:
    const char* title() const override { return "Resolution"; }

    void build(Form& f) override {
        f.heading("3D resolution");
        f.hint("The game renders at the size of its window (or of the screen in fullscreen), at any aspect ratio. These options apply at the next start.",
               true);
        f.slider(
            "Render scale", 25, 200, [](const Settings& s) { return std::clamp(s.display.render_scale, 25, 200); },
            [](Settings& s, int v) { s.display.render_scale = v; }, [](int v) { return std::to_string(v) + " %"; });
        f.hint("Percent of the window size the 3D scene is drawn at. The HUD and the menus always use the full window size.");
        f.number_pair(
            "Fixed render size", 0, 16384, [](const Settings& s) { return s.display.render_width; },
            [](Settings& s, int v) { s.display.render_width = v <= 0 ? 0 : std::clamp(v, 320, 16384); },
            [](const Settings& s) { return s.display.render_height; },
            [](Settings& s, int v) { s.display.render_height = v <= 0 ? 0 : std::clamp(v, 320, 16384); }, nullptr, "pixels");
        f.hint("0 x 0 follows the render scale. A fixed size is used for the 3D scene whatever the window size.");
        f.heading("Aspect ratio");
        f.combo(
            "Aspect ratio", {kAspectNames, kAspectNames + kAspectCount}, [](const Settings& s) { return aspect_index(s.display.aspect); },
            [](Settings& s, int i) { s.display.aspect = kAspects[std::clamp(i, 0, kAspectCount - 1)]; });
        f.hint("Follow the window fills the window at its own shape (ultrawide screens see more to the sides). A fixed ratio shows that picture "
               "inside the window with black bars.");
    }
};

std::unique_ptr<Page> make_resolution_page() { return std::make_unique<ResolutionPage>(); }

const PageFactory kTabs[] = {&make_resolution_page};

const GameLauncher& hwaoc_launcher_pages() {
    static const GameLauncher g = [] {
        GameLauncher l;
        l.tabs = kTabs;
        l.tab_count = static_cast<unsigned>(sizeof kTabs / sizeof kTabs[0]);
        return l;
    }();
    return g;
}

}  // namespace

}  // namespace nxl

void nxrt_register_game_hwaoc_launcher() {
    static const nxrt::game::Info info = [] {
        nxrt::game::Info g = nxrt::games::hwaoc::data_info();
        g.launcher = &nxl::hwaoc_launcher_pages();
        return g;
    }();
    nxrt::game::set_current(&info);
}
