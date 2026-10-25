// SPDX-License-Identifier: GPL-3.0-or-later
// The F1 page entries of Metroid Prime Remastered (play_game.h GamePlay::mouse_tab): the morph ball camera, the beam wheel, lock-on and the gun's
// hold on the view. The base page (src/gfx/play/play_ui.cpp) calls this at three places of its Mouse tab; the widgets are the ones the page had
// there before the game boundary.
#include <cmath>

#include "imgui.h"
#include "play_types.h"

namespace nxrt::play {

namespace {

// A slider on a double; true when the user changed it (the same as the base page's).
bool slider(const char* label, double* v, double lo, double hi, const char* format, ImGuiSliderFlags flags = 0) {
    float f = static_cast<float>(*v);
    if (!ImGui::SliderFloat(label, &f, static_cast<float>(lo), static_cast<float>(hi), format, flags)) return false;
    *v = f;
    return true;
}

}  // namespace

bool mpr_mouse_tab(int where, Settings& edit) {
    bool changed = false;
    MouseSettings& m = edit.mouse;
    if (where == 0) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Morph ball camera (mouse orbits the camera around the ball)");
        double src = source_sens(m.alt_yaw_rad);
        if (slider("Ball camera horizontal", &src, 0.1, 20.0, "%.2f", ImGuiSliderFlags_Logarithmic)) {
            m.alt_yaw_rad = rad_per_count(src);
            changed = true;
        }
        src = source_sens(m.alt_pitch_rad);
        if (slider("Ball camera vertical", &src, 0.1, 20.0, "%.2f", ImGuiSliderFlags_Logarithmic)) {
            m.alt_pitch_rad = rad_per_count(src);
            changed = true;
        }
    } else if (where == 1) {
        if (ImGui::Checkbox("Mouse wheel cycles beams", &m.wheel_cycles)) changed = true;
    } else if (where == 2) {
        ImGui::Spacing();
        static const char* const kLockModeLabels[] = {"Classic (the view turns to the target)", "Free (you keep control of the view)"};
        const int lock_sel = static_cast<int>(edit.aim.lock_mode);
        if (ImGui::BeginCombo("Lock-on camera", kLockModeLabels[lock_sel])) {
            for (int i = 0; i < 2; ++i) {
                if (ImGui::Selectable(kLockModeLabels[i], i == lock_sel) && i != lock_sel) {
                    edit.aim.lock_mode = static_cast<LockMode>(i);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Free: the lock starts like the game's. Afterwards the view stays under your control (mouse and right stick) as long as the target stays on screen; the gun and the reticle stay on the target. Scanning always uses classic.");
        ImGui::PopTextWrapPos();
        ImGui::BeginDisabled(edit.aim.lock_mode != LockMode::Free);
        int margin_pct = static_cast<int>(std::lround(edit.aim.free_lock_margin * 100.0));
        if (ImGui::SliderInt("Free lock edge margin", &margin_pct, 0, 40, "%d %%")) {
            edit.aim.free_lock_margin = margin_pct / 100.0;
            changed = true;
        }
        ImGui::EndDisabled();
        if (ImGui::Checkbox("Gun drives the view (the game's own: idle sway, walking bob and landing move the view)", &edit.camera.gun_drives_view)) changed = true;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("On: the arm cannon is fixed to the view, so its idle drift, walking bob and landing dip move the whole view. Off: the view follows only the mouse and the stick, and those movements move the gun on screen instead.");
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::TextWrapped("Raw mouse input, no acceleration. Aim only applies in the first-person view when not locked on, not in the morph ball and not in a cutscene. Holding the free look key (Alt) turns the view without turning Samus.");
    }
    return changed;
}

}  // namespace nxrt::play
