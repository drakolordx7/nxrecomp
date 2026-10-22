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
#include "nxrt/frame_limiter.h"
#include "nxrt/input_source.h"

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

// Takes the captured input, if the host has one. Results are taken every frame, so a stale one never reaches a later rebind.
void poll_capture(UiHost& host, Page& p) {
    InputId id;
    if (host.capture_result(&id)) {
        if (p.rebind >= 0 && p.rebind < static_cast<int>(p.edit.bindings.size())) {
            p.edit.bindings[static_cast<std::size_t>(p.rebind)].in = id;
            mark(p);
        }
        p.rebind = -1;
    } else if (p.rebind >= 0 && !host.capturing()) {
        p.rebind = -1;   // Escape, or the host ended the capture without a result
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
}

void apply_now(UiHost& host, Page& p) {
    stop_rebind(host, p);
    sanitize(p.edit);
    host.apply(p.edit);
    p.dirty = false;
}

void revert_to_saved(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit = host.settings();
    refresh_out_text(p);
    p.dirty = false;
}

void load_factory(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit = host.factory();
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
    if (ImGui::Checkbox("Invert vertical", &m.invert_y)) mark(p);
    if (ImGui::Checkbox("Invert horizontal", &m.invert_x)) mark(p);
    if (ImGui::Checkbox("Mouse wheel cycles beams", &m.wheel_beams)) mark(p);
    if (ImGui::Checkbox("Left click is a touch in menus and the map", &m.menu_touch)) mark(p);

    ImGui::Spacing();
    ImGui::TextWrapped("Raw mouse input, no acceleration. Aim only applies in the first-person view when not locked on, not in the morph ball and not in a cutscene.");
}

// ---- Controller ---------------------------------------------------------------------------------------------------------------------

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
    if (slider("Stick dead zone", &pad.stick_deadzone, 0.0, 0.5, "%.2f")) mark(p);
    if (slider("Trigger threshold", &pad.trigger_threshold, 0.05, 1.0, "%.2f")) mark(p);
    if (ImGui::Checkbox("Gyro (six-axis sensor)", &pad.gyro)) mark(p);
    if (slider("Gyro scale", &pad.gyro_scale, 0.1, 4.0, "%.2f", ImGuiSliderFlags_Logarithmic)) mark(p);
    if (ImGui::Checkbox("Rumble", &pad.rumble)) mark(p);
    if (slider("Rumble scale", &pad.rumble_scale, 0.0, 2.0, "%.2f")) mark(p);

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

void display_tab(const LiveStatus& live, Page& p) {
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

    ImGui::SeparatorText("Live");
    ImGui::Text("Window: %s, present mode: %s", cstr(live.window_mode), cstr(live.present_mode));
    ImGui::Text("Display %.2f Hz, window %d x %d, %.1f fps", live.refresh_hz, live.width, live.height, live.fps);
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
            display_tab(live, p);
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
