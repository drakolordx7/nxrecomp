// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode settings page: the "Controls" window of the F1 overlay. The contract is play_ui.h. The page edits a working copy of the
// settings: Apply and save hands it to the host, Revert and Factory defaults replace the copy, and nothing else reaches the game.
#include "play_ui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "play_glow.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/extras.h"
#include "nxrt/input_source.h"
#include "nxrt/lowlat.h"

namespace nxrt::play {

namespace {

namespace hid = nxrt::hid;

const ImVec4 kRed(1.0f, 0.35f, 0.35f, 1.0f);
const ImVec4 kYellow(1.0f, 0.85f, 0.3f, 1.0f);
const ImVec4 kOrange(1.0f, 0.65f, 0.25f, 1.0f);
const ImVec4 kPressed(0.20f, 0.60f, 0.30f, 1.0f);
const ImVec4 kReleased(0.25f, 0.25f, 0.28f, 1.0f);

constexpr int kWindowModeCount = 3;
constexpr const char* kWindowModeNames[kWindowModeCount] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
constexpr const char* kWindowModeKeys[kWindowModeCount] = {"windowed", "borderless", "exclusive"};
constexpr int kFpsChoices[] = {0, 30, 60, 72, 90, 120, 144, 165, 240, 360};
// The aspect ratio combo: Auto stores 0, the presets store their ratio, Custom shows a slider (index kAspectCustom, no value of its own).
constexpr const char* kAspectNames[] = {"Auto", "16:9", "16:10", "21:9", "32:9", "Custom"};
constexpr double kAspectValues[] = {0.0, 16.0 / 9.0, 1.6, 21.0 / 9.0, 32.0 / 9.0};
constexpr int kAspectCustom = 5;
constexpr int kAspectNameCount = static_cast<int>(sizeof kAspectNames / sizeof kAspectNames[0]);
constexpr const char* kAnisoNames[] = {"Game default", "2x", "4x", "8x", "16x"};
constexpr int kAnisoValues[] = {0, 2, 4, 8, 16};
constexpr int kAnisoCount = static_cast<int>(sizeof kAnisoValues / sizeof kAnisoValues[0]);
constexpr Trig kTrigChoices[] = {Trig::Press, Trig::Tap, Trig::Hold, Trig::Toggle, Trig::DoubleTap};
// The Button prompts combo on the Controller tab, in PromptStyle order.
constexpr const char* kPromptLabels[kPromptStyleCount] = {"Switch (the game's own)", "Xbox", "PlayStation", "Keyboard and mouse",
                                                          "Auto (follows the last used device)"};

struct SwitchButton {
    const char* name;
    hid::NpadButtonBit bit;
};
constexpr SwitchButton kSwitchButtons[] = {
    {"A", hid::kBtnA},         {"B", hid::kBtnB},         {"X", hid::kBtnX},         {"Y", hid::kBtnY},
    {"L", hid::kBtnL},         {"R", hid::kBtnR},         {"ZL", hid::kBtnZL},       {"ZR", hid::kBtnZR},
    {"PLUS", hid::kBtnPlus},   {"MINUS", hid::kBtnMinus}, {"UP", hid::kBtnUp},       {"DOWN", hid::kBtnDown},
    {"LEFT", hid::kBtnLeft},   {"RIGHT", hid::kBtnRight}, {"LSTICK", hid::kBtnStickL}, {"RSTICK", hid::kBtnStickR},
};
constexpr int kSwitchButtonCount = static_cast<int>(sizeof kSwitchButtons / sizeof kSwitchButtons[0]);
static_assert(kSwitchButtonCount == 16, "the Switch pad has 16 buttons in the status view");

// The working copy and the editing state of the page. One page per process.
struct Page {
    bool loaded = false;
    bool dirty = false;                    // the working copy differs from the settings in force (set by every change)
    Settings edit;
    Ctx ctx = Ctx::Gameplay;               // the context shown in the Bindings tab
    int rebind = -1;                       // index into edit.bindings of the row waiting for an input; -1 = none
    bool link_pitch = true;                // Mouse tab: moving the horizontal sensitivity moves the vertical one too (local, not saved)
    bool out_tab_open = false;             // the Switch layout tab was open last frame; its fields are refilled when it opens
    char out_text[kActCount][128] = {};    // the text of each output field
    std::string out_err[kActCount];        // parse error of each field; empty when its text is valid
    bool aspect_custom = false;            // Display tab: the Custom aspect entry is picked (local, not saved)
    bool fps_capture = false;              // Display tab: the frame rate counter's hotkey waits for a key (local, not saved)
};

void mark(Page& p) { p.dirty = true; }

const char* yes_no(bool b) { return b ? "yes" : "no"; }
const char* cstr(const char* s) { return s ? s : ""; }
const char* act_label(Act a) { return a == Act::None ? "none (input off)" : act_info(a).name; }

// Stops our rebind. The host's capture is cancelled only when it is ours.
void stop_rebind(UiHost& host, Page& p) {
    if (p.rebind >= 0 && host.capturing()) host.cancel_capture();
    p.rebind = -1;
}

// Ends the frame rate counter's hotkey capture. The host's capture is cancelled only when it is ours.
void stop_fps_capture(UiHost& host, Page& p) {
    if (p.fps_capture && host.capturing()) host.cancel_capture();
    p.fps_capture = false;
}

// Takes the captured input, if the host has one. Results are taken every frame, so a stale one never reaches a later rebind.
// Only one capture runs at a time (the binding row or the frame rate hotkey), so a result goes to whichever one is waiting.
void poll_capture(UiHost& host, Page& p) {
    InputId id;
    if (host.capture_result(&id)) {
        if (p.fps_capture) {
            // The frame rate hotkey takes keyboard keys only; a mouse button or a wheel notch is dropped and the capture ends.
            p.fps_capture = false;
            if (id.kind == InKind::Key) {
                Settings cur = host.settings();
                cur.fps.key = id;
                host.apply(cur);   // live: not part of the working copy
            }
        } else if (p.rebind >= 0 && p.rebind < static_cast<int>(p.edit.bindings.size())) {
            p.edit.bindings[static_cast<std::size_t>(p.rebind)].in = id;
            mark(p);
        }
        p.rebind = -1;
    } else if ((p.rebind >= 0 || p.fps_capture) && !host.capturing()) {
        p.rebind = -1;   // Escape, or the host ended the capture without a result
        p.fps_capture = false;
    }
}

// Fills the Switch layout fields from the working copy. Also clears their errors.
void refresh_out_text(Page& p) {
    for (int a = 1; a < kActCount; ++a) {   // Act::None has no row
        const std::string text = output_text(p.edit.out[a]);
        std::snprintf(p.out_text[a], sizeof p.out_text[a], "%s", text.c_str());
        p.out_err[a].clear();
    }
}

// Changes made by the page that must hold before the copy is handed to the host: no bindings without an input, values in range.
void sanitize(Settings& s) {
    std::vector<Binding>& v = s.bindings;
    v.erase(std::remove_if(v.begin(), v.end(), [](const Binding& b) { return b.in.kind == InKind::None; }), v.end());
    for (Binding& b : v) b.ms = std::clamp(b.ms, 0, 5000);
    s.display.width = std::clamp(s.display.width, 320, 16384);
    s.display.height = std::clamp(s.display.height, 320, 16384);
    // A fixed render size is 0 (follow the window) or 320..16384 per side; the settings file rejects anything else.
    s.display.render_width = s.display.render_width <= 0 ? 0 : std::clamp(s.display.render_width, 320, 16384);
    s.display.render_height = s.display.render_height <= 0 ? 0 : std::clamp(s.display.render_height, 320, 16384);
    // Extras: peak and paper white are 0 (auto) or in range; the upscale mode is kept as it is, even for the modes not yet offered.
    extras::HdrSettings& h = s.extras.hdr;
    h.peak_nits = h.peak_nits <= 0 ? 0 : std::clamp(h.peak_nits, 200.0, 10000.0);
    h.paper_white_nits = h.paper_white_nits <= 0 ? 0 : std::clamp(h.paper_white_nits, 80.0, 500.0);
    h.highlight_boost = std::clamp(h.highlight_boost, 0.0, 2.0);
    s.extras.upscale.sharpness = std::clamp(s.extras.upscale.sharpness, 0.0, 1.0);
}

// The frame rate counter is live (the hotkey changes it in the game), so the working copy always takes it from the host, never from a
// stale copy: Apply and save, Revert and Factory defaults leave the counter as it is.
void apply_now(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit.fps = host.settings().fps;
    sanitize(p.edit);
    host.apply(p.edit);
    p.dirty = false;
}

void revert_to_saved(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit = host.settings();
    p.edit.fps = host.settings().fps;
    refresh_out_text(p);
    p.dirty = false;
}

void load_factory(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit = host.factory();
    p.edit.fps = host.settings().fps;
    refresh_out_text(p);
    mark(p);
}

// A slider on a double; true when the user changed it.
bool slider(const char* label, double* v, double lo, double hi, const char* format, ImGuiSliderFlags flags = 0) {
    float f = static_cast<float>(*v);
    if (!ImGui::SliderFloat(label, &f, static_cast<float>(lo), static_cast<float>(hi), format, flags)) return false;
    *v = f;
    return true;
}

// An int field. The value is clamped when the field is left, not while typing (typing "1280" must not turn into "320" on the way).
bool int_field(const char* label, int* v, int lo, int hi, int step, int step_fast) {
    bool changed = ImGui::InputInt(label, v, step, step_fast);
    if (ImGui::IsItemDeactivated()) {
        const int c = std::clamp(*v, lo, hi);
        changed = changed || c != *v;
        *v = c;
    }
    return changed;
}

// One side of the fixed render size: 0 (follow the window) or 320..16384. A value in between is clamped to 320 when the field is left.
bool render_size_field(const char* label, int* v) {
    bool changed = ImGui::InputInt(label, v, 0, 0);
    if (ImGui::IsItemDeactivated()) {
        const int c = *v <= 0 ? 0 : std::clamp(*v, 320, 16384);
        changed = changed || c != *v;
        *v = c;
    }
    return changed;
}

// ---- Mouse --------------------------------------------------------------------------------------------------------------------------

void sensitivity_help(double rad) {
    if (rad <= 0) {
        ImGui::TextDisabled("= %.6f rad per count", rad);
        return;
    }
    ImGui::TextDisabled("= %.6f rad per count, %.3f degrees per count, 360 degrees in %.0f counts", rad, rad * 180.0 / kPi, 2.0 * kPi / rad);
}

void mouse_tab(Page& p) {
    MouseSettings& m = p.edit.mouse;
    double src = source_sens(m.yaw_rad);
    if (slider("Horizontal sensitivity", &src, 0.1, 20.0, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        m.yaw_rad = rad_per_count(src);
        if (p.link_pitch) m.pitch_rad = m.yaw_rad;
        mark(p);
    }
    sensitivity_help(m.yaw_rad);

    src = source_sens(m.pitch_rad);
    if (slider("Vertical sensitivity", &src, 0.1, 20.0, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        m.pitch_rad = rad_per_count(src);
        mark(p);
    }
    sensitivity_help(m.pitch_rad);
    ImGui::Checkbox("Link vertical to horizontal", &p.link_pitch);

    ImGui::Spacing();
    ImGui::TextUnformatted("Morph ball camera (mouse orbits the camera around the ball)");
    src = source_sens(m.morph_yaw_rad);
    if (slider("Ball camera horizontal", &src, 0.1, 20.0, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        m.morph_yaw_rad = rad_per_count(src);
        mark(p);
    }
    src = source_sens(m.morph_pitch_rad);
    if (slider("Ball camera vertical", &src, 0.1, 20.0, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        m.morph_pitch_rad = rad_per_count(src);
        mark(p);
    }

    ImGui::Spacing();
    if (ImGui::Checkbox("Invert vertical", &m.invert_y)) mark(p);
    if (ImGui::Checkbox("Invert horizontal", &m.invert_x)) mark(p);
    if (ImGui::Checkbox("Mouse wheel cycles beams", &m.wheel_beams)) mark(p);
    if (ImGui::Checkbox("Mouse pointer in menus (cursor, hover, click)", &m.menu_cursor)) mark(p);

    ImGui::Spacing();
    static const char* const kLockModeLabels[] = {"Classic (the view turns to the target)", "Free (you keep control of the view)"};
    const int lock_sel = static_cast<int>(p.edit.aim.lock_mode);
    if (ImGui::BeginCombo("Lock-on camera", kLockModeLabels[lock_sel])) {
        for (int i = 0; i < 2; ++i) {
            if (ImGui::Selectable(kLockModeLabels[i], i == lock_sel) && i != lock_sel) {
                p.edit.aim.lock_mode = static_cast<LockMode>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Free: the lock starts like the game's. Afterwards the view stays under your control (mouse and right stick) as long as the target stays on screen; the gun and the reticle stay on the target. Scanning always uses classic.");
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(p.edit.aim.lock_mode != LockMode::Free);
    int margin_pct = static_cast<int>(std::lround(p.edit.aim.free_lock_margin * 100.0));
    if (ImGui::SliderInt("Free lock edge margin", &margin_pct, 0, 40, "%d %%")) {
        p.edit.aim.free_lock_margin = margin_pct / 100.0;
        mark(p);
    }
    ImGui::EndDisabled();
    bool steady = !p.edit.camera.gun_drives_view;   // shown inverted: "Steady view" on = gun_drives_view 0
    if (ImGui::Checkbox("Steady view (gun sway and walking bob move only the gun)", &steady)) { p.edit.camera.gun_drives_view = !steady; mark(p); }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("On: the view follows only the mouse and the stick, and the idle drift, walking bob and landing dip move the gun on screen instead. Off (the game's own): the arm cannon is fixed to the view, so those movements move the whole view.");
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::TextWrapped("Raw mouse input, no acceleration. Aim only applies in the first-person view when not locked on, not in the morph ball and not in a cutscene. Holding the free look key (Alt) turns the view without turning Samus.");
}

// ---- Controller ---------------------------------------------------------------------------------------------------------------------

// The 16 controller buttons, in PadSrc order.
constexpr const char* kPadSrcLabels[kPadSrcCount] = {
    "A / Cross (bottom)", "B / Circle (right)", "X / Square (left)", "Y / Triangle (top)",
    "LB / L1",            "RB / R1",            "LT / L2",            "RT / R2",
    "Menu / Options",     "View / Create",      "Left stick click",   "Right stick click",
    "D-pad up",           "D-pad down",         "D-pad left",         "D-pad right",
};
static_assert(sizeof kPadSrcLabels / sizeof kPadSrcLabels[0] == 16, "one label per controller button");

// The choices of a controller button: Default, Nothing, then the single Switch buttons in kSwitchButtons order.
constexpr const char* kPadChoiceNames[] = {
    "Default", "Nothing",
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Left stick click", "Right stick click",
};
constexpr int kPadChoiceCount = static_cast<int>(sizeof kPadChoiceNames / sizeof kPadChoiceNames[0]);
static_assert(kPadChoiceCount == 2 + kSwitchButtonCount, "one choice per Switch button, plus Default and Nothing");
constexpr const char* kPadCustomLabel = "Custom (edit in controls.ini)";

// The value a choice stores.
uint64_t pad_choice_value(int c) {
    if (c == 0) return kPadDefault;
    if (c == 1) return 0;
    return static_cast<uint64_t>(hid::btn(kSwitchButtons[c - 2].bit));
}

// The choice a stored value shows as, or -1 when no choice holds it (a mask such as two buttons, set in the file).
int pad_choice_of(uint64_t v) {
    for (int c = 0; c < kPadChoiceCount; ++c)
        if (pad_choice_value(c) == v) return c;
    return -1;
}

void pad_buttons_group(Page& p) {
    ImGui::SeparatorText("Pad buttons");
    ImGui::TextWrapped("What each controller button presses on the Switch pad. Default keeps the built-in layout.");
    if (ImGui::BeginTable("pad_buttons", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Controller button");
        ImGui::TableSetupColumn("Presses on the Switch pad");
        ImGui::TableHeadersRow();
        for (int i = 0; i < kPadSrcCount; ++i) {
            uint64_t& v = p.edit.pad.remap[i];
            const int sel = pad_choice_of(v);
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(kPadSrcLabels[i]);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##remap", sel >= 0 ? kPadChoiceNames[sel] : kPadCustomLabel)) {
                if (sel < 0) {   // a mask no choice holds: shown as the current entry, not offered
                    ImGui::BeginDisabled();
                    ImGui::Selectable(kPadCustomLabel, true);
                    ImGui::EndDisabled();
                }
                for (int c = 0; c < kPadChoiceCount; ++c) {
                    if (ImGui::Selectable(kPadChoiceNames[c], c == sel) && c != sel) {
                        v = pad_choice_value(c);
                        mark(p);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset pad buttons")) {
        bool changed = false;
        for (uint64_t& v : p.edit.pad.remap) {
            if (v != kPadDefault) {
                v = kPadDefault;
                changed = true;
            }
        }
        if (changed) mark(p);
    }
}

void pad_tab(const LiveStatus& live, Page& p) {
    PadSettings& pad = p.edit.pad;
    const bool by_label = pad.label_abxy;
    if (ImGui::RadioButton("Buttons by label (Xbox A = Switch A)", by_label) && !by_label) {
        pad.label_abxy = true;
        mark(p);
    }
    if (ImGui::RadioButton("Buttons by position (bottom button = Switch B)", !by_label) && by_label) {
        pad.label_abxy = false;
        mark(p);
    }
    PromptSettings& prompts = p.edit.prompts;
    const int prompt_sel = static_cast<int>(prompts.style);
    if (ImGui::BeginCombo("Button prompts", kPromptLabels[prompt_sel])) {
        for (int i = 0; i < kPromptStyleCount; ++i) {
            if (ImGui::Selectable(kPromptLabels[i], i == prompt_sel) && i != prompt_sel) {
                prompts.style = static_cast<PromptStyle>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    if (prompts.style == PromptStyle::Auto) ImGui::TextDisabled("Showing now: %s", cstr(live.prompt_now));

    if (slider("Left stick dead zone", &pad.stick_deadzone, 0.0, 0.5, "%.2f")) mark(p);
    if (slider("Right stick dead zone", &pad.right_deadzone, 0.0, 0.5, "%.2f")) mark(p);
    if (ImGui::Checkbox("Invert look (right stick up looks down)", &pad.invert_y)) mark(p);
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", "Applies in play only; adds to the game's own invert option.");
    if (slider("Look sensitivity", &pad.look_sens, 0.25, 3.0, "%.2f", ImGuiSliderFlags_Logarithmic)) mark(p);
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", "Multiplies the right stick while playing. Below 1 the top turning speed is lower; above 1 you reach it with less stick.");
    if (slider("Trigger threshold", &pad.trigger_threshold, 0.05, 1.0, "%.2f")) mark(p);
    if (ImGui::Checkbox("Gyro (six-axis sensor)", &pad.gyro)) mark(p);
    if (slider("Gyro scale", &pad.gyro_scale, 0.1, 4.0, "%.2f", ImGuiSliderFlags_Logarithmic)) mark(p);
    if (ImGui::Checkbox("Rumble", &pad.rumble)) mark(p);
    if (slider("Rumble scale", &pad.rumble_scale, 0.0, 2.0, "%.2f")) mark(p);

    pad_buttons_group(p);

    ImGui::SeparatorText("Gamepad");
    if (live.pads == 0) ImGui::TextUnformatted("No gamepad open.");
    else ImGui::Text("%d open: %s", live.pads, live.pad_name.c_str());
    ImGui::Text("Gyro available: %s    Rumble available: %s", yes_no(live.pad_gyro), yes_no(live.pad_rumble));
}

// ---- Display ------------------------------------------------------------------------------------------------------------------------

int window_mode_index(const std::string& mode) {
    for (int i = 0; i < kWindowModeCount; ++i)
        if (mode == kWindowModeKeys[i]) return i;
    return 0;
}

// The combo entry for an aspect value. Auto wins for 0; Custom is shown when picked or when the value is no preset.
int aspect_index(double aspect, bool custom) {
    if (aspect <= 0.0) return 0;
    if (custom) return kAspectCustom;
    for (int i = 1; i < kAspectCustom; ++i)
        if (std::fabs(aspect - kAspectValues[i]) < 1e-6) return i;
    return kAspectCustom;
}

void fps_label(int fps, char* buf, std::size_t n) {
    if (fps <= 0) std::snprintf(buf, n, "Unlimited");
    else std::snprintf(buf, n, "%d fps", fps);
}

// The frame rate counter. Its controls act on the host at once, not on the working copy: the game's hotkey flips `show` too, so the
// page reads the host's value every frame and never writes a stale copy. These controls do not mark the page as changed.
constexpr const char* kCornerNames[] = {"Top left", "Top right", "Bottom left", "Bottom right"};   // in Corner order
constexpr int kCornerCount = static_cast<int>(sizeof kCornerNames / sizeof kCornerNames[0]);
constexpr const char* kFpsDetailNames[] = {"Frame rate", "Frame rate and frame time", "Plus 1% low and worst frame"};
constexpr int kFpsDetailCount = static_cast<int>(sizeof kFpsDetailNames / sizeof kFpsDetailNames[0]);

void set_fps_live(UiHost& host, const FpsSettings& f) {
    Settings cur = host.settings();
    cur.fps = f;
    host.apply(cur);
}

void fps_group(UiHost& host, Page& p) {
    const FpsSettings fps = host.settings().fps;
    ImGui::SeparatorText("Frame rate counter");

    bool show = fps.show;
    if (ImGui::Checkbox("Show frame rate counter", &show)) {
        FpsSettings f = fps;
        f.show = show;
        set_fps_live(host, f);
    }

    const int corner = static_cast<int>(fps.corner);
    if (ImGui::BeginCombo("Corner##fps_corner", kCornerNames[corner])) {
        for (int i = 0; i < kCornerCount; ++i) {
            if (ImGui::Selectable(kCornerNames[i], i == corner) && i != corner) {
                FpsSettings f = fps;
                f.corner = static_cast<Corner>(i);
                set_fps_live(host, f);
            }
        }
        ImGui::EndCombo();
    }

    const int detail = std::clamp(fps.detail, 0, kFpsDetailCount - 1);
    if (ImGui::BeginCombo("Detail##fps_detail", kFpsDetailNames[detail])) {
        for (int i = 0; i < kFpsDetailCount; ++i) {
            if (ImGui::Selectable(kFpsDetailNames[i], i == detail) && i != detail) {
                FpsSettings f = fps;
                f.detail = i;
                set_fps_live(host, f);
            }
        }
        ImGui::EndCombo();
    }

    bool shots = fps.in_screenshots;
    if (ImGui::Checkbox("Also draw it in screenshots", &shots)) {
        FpsSettings f = fps;
        f.in_screenshots = shots;
        set_fps_live(host, f);
    }

    const std::string key = input_name(fps.key);
    ImGui::Text("Hotkey: %s", key.empty() ? "?" : key.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Change##fps_key")) {
        stop_rebind(host, p);
        p.fps_capture = true;
        host.begin_capture();
    }
    if (p.fps_capture) {
        ImGui::SameLine();
        ImGui::TextDisabled("press a keyboard key...");
    }
}

void display_tab(UiHost& host, const LiveStatus& live, Page& p) {
    DisplaySettings& d = p.edit.display;
    const int mode = window_mode_index(d.window_mode);
    if (ImGui::BeginCombo("Window mode", kWindowModeNames[mode])) {
        for (int i = 0; i < kWindowModeCount; ++i) {
            if (ImGui::Selectable(kWindowModeNames[i], i == mode) && i != mode) {
                d.window_mode = kWindowModeKeys[i];
                mark(p);
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::Checkbox("Vsync", &d.vsync)) mark(p);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Off: no forced vsync, lowest latency, works well with G-Sync / FreeSync together with a frame cap. On: the display paces the game.");
    ImGui::PopTextWrapPos();

    char cap[32];
    fps_label(d.fps_cap, cap, sizeof cap);
    if (ImGui::BeginCombo("Frame cap", cap)) {
        for (int fps : kFpsChoices) {
            char name[32];
            fps_label(fps, name, sizeof name);
            if (ImGui::Selectable(name, fps == d.fps_cap) && fps != d.fps_cap) {
                d.fps_cap = fps;
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(live.refresh_hz <= 0.0f);
    if (ImGui::Button("Just under the refresh rate")) {
        d.fps_cap = static_cast<int>(std::floor(nxrt::suggest_vrr_cap(live.refresh_hz)));
        mark(p);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (live.refresh_hz > 0.0f) ImGui::TextDisabled("display %.2f Hz", live.refresh_hz);
    else ImGui::TextDisabled("display refresh rate unknown");

    if (ImGui::Checkbox("Keep the 16:9 picture, letterbox the rest", &d.keep_aspect)) mark(p);
    if (int_field("Window width", &d.width, 320, 16384, 1, 100)) mark(p);
    if (int_field("Window height", &d.height, 320, 16384, 1, 100)) mark(p);
    ImGui::TextDisabled("The windowed size applies at the next start.");

    ImGui::SeparatorText("Image");
    if (ImGui::SliderInt("Render scale (restart)", &d.render_scale, 25, 400, "%d%%")) mark(p);
    ImGui::TextUnformatted("Fixed render size, 0 = follow the window (restart)");
    if (render_size_field("Width##render_w", &d.render_width)) mark(p);
    ImGui::SameLine();
    if (render_size_field("Height##render_h", &d.render_height)) mark(p);

    const int aspect_sel = aspect_index(d.aspect, p.aspect_custom);
    if (ImGui::BeginCombo("Aspect ratio (restart)", kAspectNames[aspect_sel])) {
        for (int i = 0; i < kAspectNameCount; ++i) {
            if (ImGui::Selectable(kAspectNames[i], i == aspect_sel) && i != aspect_sel) {
                if (i == kAspectCustom) {
                    p.aspect_custom = true;
                    if (d.aspect <= 0.0) d.aspect = kAspectValues[1];   // Custom starts at 16:9
                } else {
                    p.aspect_custom = false;
                    d.aspect = kAspectValues[i];                       // Auto is index 0, value 0
                }
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    if (aspect_sel == kAspectCustom && slider("Custom aspect ratio", &d.aspect, 1.0, 5.0, "%.2f")) mark(p);
    ImGui::TextDisabled("Resolution options apply the next time the game starts.");

    if (slider("FOV offset (degrees, vertical)", &d.fov_offset, -30.0, 40.0, "%.1f")) mark(p);
    if (ImGui::Checkbox("Dynamic resolution (lowers the render size when the GPU is late)", &d.dynamic_resolution)) mark(p);
    int aniso_sel = 0;
    for (int i = 0; i < kAnisoCount; ++i)
        if (kAnisoValues[i] == d.anisotropy) aniso_sel = i;
    if (ImGui::BeginCombo("Texture anisotropy", kAnisoNames[aniso_sel])) {
        for (int i = 0; i < kAnisoCount; ++i) {
            if (ImGui::Selectable(kAnisoNames[i], i == aniso_sel) && i != aniso_sel) {
                d.anisotropy = kAnisoValues[i];
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    if (slider("Texture LOD bias", &d.lod_bias, -2.0, 2.0, "%.2f")) mark(p);
    if (ImGui::Checkbox("Bloom", &d.bloom)) mark(p);
    if (ImGui::Checkbox("Chromatic aberration", &d.chromatic_aberration)) mark(p);
    if (ImGui::Checkbox("Faster GPU memory (uses more video memory) (restart)", &d.fast_gpu_memory)) mark(p);
    if (ImGui::Checkbox("Fewer GPU pipeline waits (restart)", &d.fewer_gpu_waits)) mark(p);

    fps_group(host, p);

    ImGui::SeparatorText("Live");
    ImGui::Text("Window: %s, present mode: %s", cstr(live.window_mode), cstr(live.present_mode));
    ImGui::Text("Display %.2f Hz, window %d x %d, %.1f fps", live.refresh_hz, live.width, live.height, live.fps);
}

// ---- Extras -------------------------------------------------------------------------------------------------------------------------

// HDR output, upscaling and low latency (nxrt/extras.h). Items that apply at the next start say so in their labels, as the Display tab does.
constexpr const char* kHdrModeNames[] = {"Off", "Auto", "On"};
constexpr const char* kHdrFormatNames[] = {"Auto", "HDR10", "scRGB"};
constexpr const char* kLatencyModeNames[] = {"Off", "Auto", "On", "Boost"};
constexpr const char* kUpscaleNames[] = {"Off", "FSR 1 (spatial)", "FSR 3", "DLSS", "XeSS"};
constexpr int kUpscaleCount = static_cast<int>(sizeof kUpscaleNames / sizeof kUpscaleNames[0]);

// Only Off and FSR 1 can be picked for now. The other modes stay in the list, greyed out, and a file that holds one shows it as selected.
bool upscale_available(extras::UpscaleMode m) { return m == extras::UpscaleMode::Off || m == extras::UpscaleMode::Fsr1; }

// A hover tooltip for the item drawn last.
void tip(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", text);
}

// A nits field where 0 means "auto" (shown as "auto"). Other values are clamped into lo..hi when the field is left, so the slider can be
// dragged through the gap below lo while it is held.
bool auto_nits_slider(const char* label, double* v, int lo, int hi) {
    int x = static_cast<int>(std::lround(*v));
    bool changed = ImGui::SliderInt(label, &x, 0, hi, x <= 0 ? "auto" : "%d nits");
    if (ImGui::IsItemDeactivated()) {
        const int c = x <= 0 ? 0 : std::clamp(x, lo, hi);
        changed = changed || c != x;
        x = c;
    }
    if (changed) *v = x;
    return changed;
}

void hdr_group(Page& p) {
    extras::HdrSettings& h = p.edit.extras.hdr;
    ImGui::SeparatorText("HDR output");

    const int mode = static_cast<int>(h.mode);
    const bool mode_open = ImGui::BeginCombo("Mode (restart)##hdr_mode", kHdrModeNames[mode]);
    tip("Off keeps the SDR picture, Auto uses HDR when the display and Windows offer it, On runs the HDR pipeline even without an HDR display.");
    if (mode_open) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(kHdrModeNames[i], i == mode) && i != mode) {
                h.mode = static_cast<extras::HdrMode>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }

    const int format = static_cast<int>(h.format);
    const bool format_open = ImGui::BeginCombo("Format (restart)##hdr_format", kHdrFormatNames[format]);
    tip("The signal sent to the display: HDR10, or scRGB; Auto picks HDR10 when the display offers it and scRGB otherwise.");
    if (format_open) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(kHdrFormatNames[i], i == format) && i != format) {
                h.format = static_cast<extras::HdrFormat>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }

    if (auto_nits_slider("Peak brightness##hdr_peak", &h.peak_nits, 200, 10000)) mark(p);
    tip("How bright the brightest highlights get, in nits; auto reads the peak brightness the display reports.");
    if (auto_nits_slider("Paper white##hdr_paper", &h.paper_white_nits, 80, 500)) mark(p);
    tip("The brightness of ordinary white in the game, in nits; auto follows Windows' SDR content brightness when HDR is on there, else 203.");
    if (slider("Highlight boost", &h.highlight_boost, 0.0, 2.0, "%.2f")) mark(p);
    tip("0 keeps the game's SDR look up to its own white, 1 lets highlights reach the display's peak, 2 pushes them further.");

    const extras::HdrState hs = extras::hdr_state();
    ImGui::Text("HDR scene: %s, swapchain: %s, paper white %.0f nits, peak %.0f nits, peak ratio %.2f, display reports HDR: %s",
                hs.scene_hdr ? "on" : "off", extras::output_kind_name(hs.output), static_cast<double>(hs.paper_white_nits),
                static_cast<double>(hs.peak_nits), static_cast<double>(hs.peak_ratio), yes_no(hs.display_hdr));
}

void upscale_group(Page& p) {
    extras::UpscaleSettings& u = p.edit.extras.upscale;
    ImGui::SeparatorText("Upscaling");

    const int mode = static_cast<int>(u.mode);
    std::string preview = kUpscaleNames[mode];
    if (!upscale_available(u.mode)) preview += " (not available yet)";
    const bool open = ImGui::BeginCombo("Mode##upscale_mode", preview.c_str());
    tip("FSR 1 draws the game below the window size and sharpens it up to the window; the other modes are not available yet.");
    if (open) {
        for (int i = 0; i < kUpscaleCount; ++i) {
            const bool ok = upscale_available(static_cast<extras::UpscaleMode>(i));
            std::string label = kUpscaleNames[i];
            if (!ok) label += " (not available yet)";
            ImGui::BeginDisabled(!ok);
            const bool picked = ImGui::Selectable(label.c_str(), i == mode);
            ImGui::EndDisabled();
            if (picked && i != mode) {
                u.mode = static_cast<extras::UpscaleMode>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }

    if (slider("Sharpness", &u.sharpness, 0.0, 1.0, "%.2f")) mark(p);
    tip("How much FSR 1 sharpens the upscaled picture; 1 is the sharpest.");
    if (ImGui::SliderInt("Scene resolution##upscale_scene", &u.scene_scale, 25, 100, "%d%%")) mark(p);
    tip("Renders the 3D scene at this percent of the output size per side; the game scales it up and keeps the HUD and the post effects at full size. 100 turns it off. Applies live.");
    ImGui::TextDisabled("Or render everything below the window size with Display > Render scale (restart); FSR 1 then fills the rest.");
}

void latency_group(Page& p) {
    extras::LatencySettings& l = p.edit.extras.latency;
    ImGui::SeparatorText("Low latency");

    const int mode = static_cast<int>(l.mode);
    const bool open = ImGui::BeginCombo("Mode (restart)##latency_mode", kLatencyModeNames[mode]);
    tip("Lowers the delay from input to picture with NVIDIA Reflex or AMD Anti-Lag 2, whichever the GPU has; Boost asks Reflex for its boost mode.");
    if (open) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(kLatencyModeNames[i], i == mode) && i != mode) {
                l.mode = static_cast<extras::LatencyMode>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::SliderInt("Frames ahead (restart)##latency_ahead", &l.frames_ahead, -1, 3, l.frames_ahead < 0 ? "no limit" : "%d")) mark(p);
    tip("With the mode on, the game starts simulating a frame only when at most this many earlier frames are still on their way to the screen. 1 keeps the game's frame queue short; 0 is the shortest delay and costs frame rate.");
    if (ImGui::Checkbox("Log timings (restart)##latency_log", &l.log_timings)) mark(p);
    tip("Writes one line of latency timings to the log every second.");

    if (const extras::LowLatency* ll = extras::lowlat()) {
        const extras::LowLatency::Stats st = ll->stats();
        ImGui::Text("%s: enabled %s, input to present %.2f ms, driver input to GPU end %.2f ms, sleep %.2f ms", ll->backend_name(),
                    yes_no(st.enabled), st.input_to_present_ms, st.input_to_gpu_ms, st.sleep_ms_avg);
    } else {
        ImGui::TextDisabled("Not running: the renderer has not started its low-latency part yet, or the mode is off.");
    }
}

void extras_tab(Page& p) {
    hdr_group(p);
    upscale_group(p);
    latency_group(p);
    if (fastboot_ui(p.edit.fastboot)) mark(p);   // start-up and menu tweaks (play_ui_fastboot.cpp)
}

// ---- Bindings -----------------------------------------------------------------------------------------------------------------------

std::string input_button_text(const Binding& b, bool waiting) {
    if (waiting) return "press a key...";
    if (b.in.kind == InKind::None) return "<none>";
    const std::string name = input_name(b.in);
    return name.empty() ? "?" : name;
}

void bindings_tab(UiHost& host, Page& p) {
    if (ImGui::BeginCombo("Context", ctx_name(p.ctx))) {
        for (int c = 0; c < kCtxCount; ++c) {
            const Ctx ctx = static_cast<Ctx>(c);
            if (ImGui::Selectable(ctx_name(ctx), ctx == p.ctx) && ctx != p.ctx) {
                stop_rebind(host, p);
                p.ctx = ctx;
            }
        }
        ImGui::EndCombo();
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    int remove = -1;
    if (ImGui::BeginTable("bindings", 5, kFlags, ImVec2(0.0f, 320.0f))) {
        ImGui::TableSetupColumn("Input");
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Trigger", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, 32.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < p.edit.bindings.size(); ++i) {
            Binding& b = p.edit.bindings[i];
            if (b.ctx != p.ctx) continue;
            const int row = static_cast<int>(i);
            ImGui::PushID(row);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            const bool waiting = p.rebind == row;
            const std::string label = input_button_text(b, waiting) + "##in";
            if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f))) {
                stop_fps_capture(host, p);
                p.rebind = row;
                host.begin_capture();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##act", act_label(b.act))) {
                for (int a = 0; a < kActCount; ++a) {
                    const Act act = static_cast<Act>(a);
                    if (ImGui::Selectable(act_label(act), act == b.act) && act != b.act) {
                        b.act = act;
                        mark(p);
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##trig", trig_name(b.trig))) {
                for (Trig t : kTrigChoices) {
                    if (ImGui::Selectable(trig_name(t), t == b.trig) && t != b.trig) {
                        b.trig = t;
                        mark(p);
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(3);
            const bool timed = b.trig == Trig::Tap || b.trig == Trig::Hold || b.trig == Trig::DoubleTap;
            ImGui::BeginDisabled(!timed);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (int_field("##ms", &b.ms, 0, 5000, 0, 0)) mark(p);
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("x")) remove = row;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (remove >= 0) {
        if (p.rebind == remove) stop_rebind(host, p);
        else if (p.rebind > remove) --p.rebind;
        p.edit.bindings.erase(p.edit.bindings.begin() + remove);
        mark(p);
    }

    if (ImGui::Button("Add binding")) {
        Binding b;
        b.in = InputId{};
        b.ctx = p.ctx;
        b.act = Act::Fire;
        b.trig = Trig::Press;
        b.ms = 0;
        p.edit.bindings.push_back(b);
        stop_fps_capture(host, p);
        p.rebind = static_cast<int>(p.edit.bindings.size()) - 1;
        host.begin_capture();
        mark(p);
    }
    ImGui::TextWrapped("Bindings of a context win over the 'all' layer for the same input. Action 'none' switches an input off in that context.");
}

// ---- Switch layout ------------------------------------------------------------------------------------------------------------------

void switch_tab(Page& p) {
    ImGui::TextWrapped("What each action presses on the Switch pad: buttons A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK "
                       "and stick directions LS_UP LS_DOWN LS_LEFT LS_RIGHT RS_UP RS_DOWN RS_LEFT RS_RIGHT, joined by +. "
                       "beam_next and beam_prev have no fixed output (they pick beam_1 to beam_4).");
    if (ImGui::BeginTable("switch_out", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Output");
        ImGui::TableHeadersRow();
        for (int a = 1; a < kActCount; ++a) {
            const bool bad = !p.out_err[a].empty();
            ImGui::PushID(a);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(act_info(static_cast<Act>(a)).name);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (bad) {
                ImGui::PushStyleColor(ImGuiCol_Border, kRed);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            }
            if (ImGui::InputText("##out", p.out_text[a], sizeof p.out_text[a])) {
                Output o;
                std::string err;
                if (parse_output(p.out_text[a], &o, &err)) {
                    p.edit.out[a] = o;
                    p.out_err[a].clear();
                    mark(p);
                } else {
                    p.out_err[a] = err;   // the previous valid output stays in the working copy
                }
            }
            if (bad) {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
            if (!p.out_err[a].empty()) ImGui::TextColored(kRed, "%s", p.out_err[a].c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset this table to the defaults")) {
        for (int a = 1; a < kActCount; ++a) {
            Output o;
            if (parse_output(act_info(static_cast<Act>(a)).default_out, &o)) p.edit.out[a] = o;
        }
        refresh_out_text(p);
        mark(p);
    }
}

// ---- Status -------------------------------------------------------------------------------------------------------------------------

// Live view only: nothing here touches the working copy.
void status_tab(const LiveStatus& live) {
    ImGui::Text("Context: %s", ctx_name(live.ctx));
    ImGui::Text("Aim: %s", cstr(live.aim));
    ImGui::Text("Mouse captured: %s", yes_no(live.mouse_captured));
    ImGui::Text("Pitch: %.1f degrees", live.pitch_deg);
    if (live.input_calls == 0) ImGui::TextDisabled("ProcessInput calls: 0 (the game has not reached gameplay yet)");
    else ImGui::Text("ProcessInput calls: %llu", static_cast<unsigned long long>(live.input_calls));

    ImGui::SeparatorText("Switch pad output");
    for (int i = 0; i < kSwitchButtonCount; ++i) {
        const bool on = (live.out.buttons & hid::btn(kSwitchButtons[i].bit)) != 0;
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? kPressed : kReleased);
        ImGui::Button(kSwitchButtons[i].name, ImVec2(56.0f, 0.0f));
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (i % 8 != 7) ImGui::SameLine();
    }
    ImGui::Text("LS (%5.2f, %5.2f)   RS (%5.2f, %5.2f)", live.out.lx, live.out.ly, live.out.rx, live.out.ry);

    ImGui::SeparatorText("Gamepad");
    if (live.pads == 0) ImGui::TextUnformatted("No gamepad open.");
    else ImGui::Text("%d open: %s", live.pads, live.pad_name.c_str());
    ImGui::Text("Gyro: %s    Rumble: %s", yes_no(live.pad_gyro), yes_no(live.pad_rumble));
}

}  // namespace

void draw_settings_page(UiHost& host) {
    static Page page;
    Page& p = page;
    if (!p.loaded) {
        p.edit = host.settings();
        p.edit.fps = host.settings().fps;
        refresh_out_text(p);
        p.loaded = true;
    }
    poll_capture(host, p);
    const LiveStatus live = host.status();

    if (host.capturing()) ImGui::TextColored(kYellow, "Press the key or mouse button for the binding, Escape cancels");
    ImGui::Text("Settings file: %s", host.settings_path().c_str());
    if (p.dirty) {
        ImGui::SameLine();
        ImGui::TextColored(kOrange, "unsaved changes");
    }
    if (ImGui::Button("Apply and save")) apply_now(host, p);
    ImGui::SameLine();
    if (ImGui::Button("Revert")) revert_to_saved(host, p);
    ImGui::SameLine();
    if (ImGui::Button("Factory defaults")) load_factory(host, p);
    ImGui::Separator();

    if (ImGui::BeginTabBar("settings_tabs")) {
        if (ImGui::BeginTabItem("Mouse")) {
            mouse_tab(p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controller")) {
            pad_tab(live, p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display")) {
            display_tab(host, live, p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Extras")) {
            extras_tab(p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Tweaks")) {
            if (glow_tab(p.edit)) mark(p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Bindings")) {
            bindings_tab(host, p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Switch layout")) {
            if (!p.out_tab_open) refresh_out_text(p);
            p.out_tab_open = true;
            switch_tab(p);
            ImGui::EndTabItem();
        } else {
            p.out_tab_open = false;
        }
        if (ImGui::BeginTabItem("Status")) {
            status_tab(live);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

}  // namespace nxrt::play
