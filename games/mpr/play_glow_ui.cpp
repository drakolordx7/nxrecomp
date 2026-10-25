// SPDX-License-Identifier: GPL-3.0-or-later
// S44: the F1 page's Tweaks tab (play_ui.cpp): the glow colour. The game shows the values while the tab is open, before
// they are saved (glow_preview); closing the page without saving goes back to the saved colours.
#include "imgui.h"
#include "mpr/mpr_tint.h"
#include "play_glow.h"

namespace nxrt::play {

namespace {

// Three 0..255 sliders and a swatch; true when the colour changed.
bool rgb_sliders(const char* id, GlowRgb* c) {
    bool changed = false;
    ImGui::PushID(id);
    int v[3] = {c->r, c->g, c->b};
    const char* names[3] = {"Red", "Green", "Blue"};
    for (int i = 0; i < 3; ++i) {
        if (ImGui::SliderInt(names[i], &v[i], 0, 255)) changed = true;
    }
    if (changed) {
        c->r = static_cast<uint8_t>(v[0]);
        c->g = static_cast<uint8_t>(v[1]);
        c->b = static_cast<uint8_t>(v[2]);
    }
    ImGui::ColorButton("##swatch", ImVec4(c->r / 255.f, c->g / 255.f, c->b / 255.f, 1.f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha,
                       ImVec2(120.f, 28.f));
    ImGui::SameLine();
    ImGui::Text("%u, %u, %u", static_cast<unsigned>(c->r), static_cast<unsigned>(c->g), static_cast<unsigned>(c->b));
    ImGui::PopID();
    return changed;
}

bool part_combo(const char* label, GlowPart* p) {
    static const char* const kItems[] = {"Use the glow colour above", "The game's own colour", "Own colour"};
    int sel = static_cast<int>(p->mode);
    bool changed = false;
    if (ImGui::Combo(label, &sel, kItems, 3) && sel != static_cast<int>(p->mode)) {
        p->mode = static_cast<GlowMode>(sel);
        changed = true;
    }
    if (p->mode == GlowMode::Color && rgb_sliders(label, &p->rgb)) changed = true;
    return changed;
}

}  // namespace

bool glow_tab(Settings& edit) {
    TweakSettings& t = edit.tweaks;
    bool changed = false;
    ImGui::TextUnformatted("Glow colour");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Recolours the morph ball's glow (its glowing stripes, inner glow, boost glow, trail, light and morph flash), the arm cannon's charge glow and lit panels, and the suit's lights. The game shows the colour at once; Apply and save keeps it.");
    ImGui::PopTextWrapPos();
    if (ImGui::Checkbox("Use a custom glow colour", &t.glow_on)) changed = true;
    ImGui::BeginDisabled(!t.glow_on);
    if (rgb_sliders("glow", &t.glow)) changed = true;
    ImGui::EndDisabled();
    if (ImGui::Button("Reset to the game's colours")) {
        const TweakSettings def;
        t.glow_on = def.glow_on;
        t.glow = def.glow;
        t.suit = def.suit;
        t.ball = def.ball;
        t.gun = def.gun;
        changed = true;
    }
    ImGui::Spacing();
    if (ImGui::TreeNode("Per part")) {
        if (part_combo("Suit", &t.suit)) changed = true;
        if (part_combo("Morph ball", &t.ball)) changed = true;
        if (part_combo("Arm cannon", &t.gun)) changed = true;
        ImGui::TreePop();
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("The colour multiplies the game's glow: the ball's white glow takes it exactly, the charge's yellow mixes with it (magenta gives a pink-red charge), and a darker colour gives a dimmer glow. The cannon's lit panels and the suit's lights take the colour's hue at the game's brightness, with every beam.");
    ImGui::PopTextWrapPos();
    glow_preview(t);

    // S45: suit and arm cannon paint (texture recolour, games/mpr/mpr_tint.h)
    ImGui::Separator();
    ImGui::TextUnformatted("Suit and arm cannon colour");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Repaints the suit (the morph ball follows it) and the arm cannon's shell in any colour from black to white, keeping the shading and detail of the game's textures. The visor, the lights and the glow keep their colours. The game shows a change about a quarter of a second after the slider stops.");
    ImGui::PopTextWrapPos();
    if (ImGui::Checkbox("Suit colour", &t.suit_color_on)) changed = true;
    ImGui::BeginDisabled(!t.suit_color_on);
    if (rgb_sliders("suit_color", &t.suit_color)) changed = true;
    ImGui::EndDisabled();
    if (ImGui::Checkbox("Arm cannon colour", &t.gun_color_on)) changed = true;
    ImGui::BeginDisabled(!t.gun_color_on);
    if (rgb_sliders("gun_color", &t.gun_color)) changed = true;
    ImGui::EndDisabled();
    if (ImGui::Button("Reset to the game's paint")) {
        const TweakSettings def;
        t.suit_color_on = def.suit_color_on;
        t.suit_color = def.suit_color;
        t.gun_color_on = def.gun_color_on;
        t.gun_color = def.gun_color;
        changed = true;
    }
    tint_preview(t);
    return changed;
}

}  // namespace nxrt::play
