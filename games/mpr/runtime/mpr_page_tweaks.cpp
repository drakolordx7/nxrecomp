// SPDX-License-Identifier: GPL-3.0-or-later
// Tweaks tab: optional looks ([tweaks] in controls.ini, play_tweaks.h), off by default. S44: the glow colour, three 0..255 sliders with a
// colour swatch and a Reset button. A part with its own colour in the file (ball_glow / gun_glow = R,G,B) keeps it; the page only switches a
// part between following the glow colour and the game's own. S45: the suit and arm cannon colour (suit_color, gun_color), each a check box,
// three sliders and a swatch like the glow, the three colours side by side, with their own Reset.
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <string>

#include "nxrt/game/launcher_ui.h"
#include "mpr_launcher.h"
#include "mpr_settings.h"

namespace nxl {

namespace {

using nxrt::play::GlowMode;
using nxrt::play::GlowRgb;
using nxrt::play::TweakSettings;

std::string byte_text(int v) { return std::to_string(v); }

// The part's drop-down: 0 follows the glow colour, 1 the game's own; a part with its own colour (only in the file) shows as 0 and is kept.
int part_index(GlowMode m) { return m == GlowMode::Off ? 1 : 0; }
void set_part(nxrt::play::GlowPart* p, int i) {
    if (i == 1) p->mode = GlowMode::Off;
    else if (p->mode == GlowMode::Off) p->mode = GlowMode::Auto;
}

class TweaksPage : public Page {
public:
    const char* title() const override { return "Tweaks"; }

    void build(Form& f) override {
        // the page does not scroll: the three colours side by side, the glow (S44), the suit's paint and the arm cannon's paint (S45)
        f.heading("Glow and paint colours");
        f.compact(true);
        f.columns(3);
        f.checkbox(
            "Glow colour", [](const Settings& s) { return mpr_settings(s).tweaks.glow_on; }, [](Settings& s, bool v) { mpr_settings(s).tweaks.glow_on = v; });
        f.checkbox(
            "Suit colour", [](const Settings& s) { return mpr_settings(s).tweaks.suit_color_on; }, [](Settings& s, bool v) { mpr_settings(s).tweaks.suit_color_on = v; });
        f.checkbox(
            "Arm cannon colour", [](const Settings& s) { return mpr_settings(s).tweaks.gun_color_on; }, [](Settings& s, bool v) { mpr_settings(s).tweaks.gun_color_on = v; });
        rgb_rows(f, {&glow_, &suit_, &gun_}, {&TweakSettings::glow, &TweakSettings::suit_color, &TweakSettings::gun_color});
        reset_id_ = f.button("Reset the glow", 150, true);
        reset_paint_id_ = f.button("Reset the paint", 150, true);
        f.compact(false);
        f.columns(1);
        f.hint("Glow: the morph ball's glow (stripes, inner glow, boost glow, trail, light), the arm cannon's charge glow and lit panels, and the suit's lights; the colour multiplies the game's glow. Paint: the suit (the morph ball follows it) and the arm cannon's shell, black to white, keeping the game's shading and detail; the visor and the lights keep their colours. Off: the game's own.", true);

        f.heading("Glow per part");
        f.columns(3);
        f.combo(
            "Suit", {"Glow colour", "The game's own"}, [](const Settings& s) { return part_index(mpr_settings(s).tweaks.suit.mode); },
            [](Settings& s, int i) { set_part(&mpr_settings(s).tweaks.suit, i); });
        f.combo(
            "Morph ball", {"Glow colour", "The game's own"}, [](const Settings& s) { return part_index(mpr_settings(s).tweaks.ball.mode); },
            [](Settings& s, int i) { set_part(&mpr_settings(s).tweaks.ball, i); });
        f.combo(
            "Arm cannon", {"Glow colour", "The game's own"}, [](const Settings& s) { return part_index(mpr_settings(s).tweaks.gun.mode); },
            [](Settings& s, int i) { set_part(&mpr_settings(s).tweaks.gun, i); });
        f.columns(1);
        f.hint("A part can also get its own glow colour in controls.ini: [tweaks] suit_glow, ball_glow or gun_glow = R,G,B.", true);
    }

    void load(const Settings& s) override {
        loaded_ = s;
        Page::load(s);
    }

    bool command(int id) override {
        if (id != reset_id_ && id != reset_paint_id_) return false;
        Settings s = loaded_;
        store(&s);
        const TweakSettings def;
        if (id == reset_id_) {
            mpr_settings(s).tweaks.glow_on = def.glow_on;
            mpr_settings(s).tweaks.glow = def.glow;
            mpr_settings(s).tweaks.suit = def.suit;
            mpr_settings(s).tweaks.ball = def.ball;
            mpr_settings(s).tweaks.gun = def.gun;
        } else {
            mpr_settings(s).tweaks.suit_color_on = def.suit_color_on;
            mpr_settings(s).tweaks.suit_color = def.suit_color;
            mpr_settings(s).tweaks.gun_color_on = def.gun_color_on;
            mpr_settings(s).tweaks.gun_color = def.gun_color;
        }
        Page::load(s);
        host()->changed();
        return true;
    }

private:
    struct RgbRows {
        Row *r = nullptr, *g = nullptr, *b = nullptr;
        uint32_t rgb() const { return (uint32_t(r ? r->value() : 255) << 16) | (uint32_t(g ? g->value() : 255) << 8) | uint32_t(b ? b->value() : 255); }
    };
    using RgbField = GlowRgb TweakSettings::*;

    static Form::GetInt channel_get(RgbField field, int ch) {
        return [field, ch](const Settings& s) {
            const GlowRgb& c = mpr_settings(s).tweaks.*field;
            return int(ch == 0 ? c.r : ch == 1 ? c.g : c.b);
        };
    }
    static Form::SetInt channel_set(RgbField field, int ch) {
        return [field, ch](Settings& s, int v) {
            GlowRgb& c = mpr_settings(s).tweaks.*field;
            (ch == 0 ? c.r : ch == 1 ? c.g : c.b) = uint8_t(v);
        };
    }

    // Red, Green and Blue sliders (0..255) and a swatch for each colour, one colour per column of the form (rows fill left to right).
    void rgb_rows(Form& f, std::initializer_list<RgbRows*> rows, std::initializer_list<RgbField> fields) {
        const char* names[3] = {"Red", "Green", "Blue"};
        for (int ch = 0; ch < 3; ++ch) {
            auto field = fields.begin();
            for (RgbRows* r : rows) {
                Row* h = f.slider(names[ch], 0, 255, channel_get(*field, ch), channel_set(*field, ch), byte_text);
                (ch == 0 ? r->r : ch == 1 ? r->g : r->b) = h;
                ++field;
            }
        }
        for (RgbRows* r : rows) f.swatch("Colour", [r]() { return r->rgb(); }, 28);
    }

    RgbRows glow_, suit_, gun_;
    int reset_id_ = -1, reset_paint_id_ = -1;
    Settings loaded_;
};

}

std::unique_ptr<Page> make_tweaks_page() { return std::make_unique<TweaksPage>(); }

}
