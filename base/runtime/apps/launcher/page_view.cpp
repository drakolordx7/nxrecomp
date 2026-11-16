// SPDX-License-Identifier: GPL-3.0-or-later
#include "page_view.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <string>

#include "nxui/ui.h"

namespace nxl {

namespace {

constexpr float kMargin = 16.f;     // logical px
constexpr float kLabelW = 200.f;    // label column of a one-column form
constexpr float kColGap = 16.f;
constexpr float kRowGap = 6.f;

int parse_int(const std::string& s) {
    long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') continue;
        v = v * 10 + (c - '0');
        if (v > 999999) break;
    }
    return static_cast<int>(v);
}

struct Ctx {
    nxui::App& app;
    Page& page;
    Host& host;
    float W = 0;       // content width
    float x0 = 0;      // left edge (window coordinates)
    bool in_line = false;
    float line_y = 0, line_h = 0;
};

struct Cell {
    float x, w, label_w;   // window coordinates and widths of the cell, and its label column
    float control_x() const { return x + label_w; }
    float control_w() const { return w - label_w; }
};

Cell cell_of(const Ctx& c, const Row& r) {
    Cell k;
    if (r.cols <= 1) {
        k.w = c.W;
        k.x = c.x0;
        k.label_w = c.app.S(kLabelW);
    } else {
        const float gap = c.app.S(kColGap);
        k.w = (c.W - (r.cols - 1) * gap) / r.cols;
        k.x = c.x0 + r.col * (k.w + gap);
        k.label_w = r.compact ? k.w * 0.24f : k.w * (r.cols == 2 ? 0.40f : 0.44f);
    }
    return k;
}

void label_text(const std::string& text, float x, float y) {
    if (text.empty()) return;
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text.c_str());
    ImGui::SetCursorPos(ImVec2(x, y));
}

void end_line(Ctx& c) {
    if (!c.in_line) return;
    ImGui::SetCursorPos(ImVec2(c.x0, c.line_y + c.line_h));
    c.in_line = false;
}

void mark_flash(Row& r) {
    if (r.flash_frames <= 0) return;
    if (r.flash_frames == 180) ImGui::SetScrollHereY(0.5f);
    --r.flash_frames;
    if ((r.flash_frames / 20) % 2 == 0) {
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(nxui::col::accent), 3.f, 0, 2.f);
    }
}

void changed(Ctx& c, Row& r) {
    if (r.after) r.after();
    c.host.changed();
}

void draw_row(Ctx& c, Row& r) {
    using K = Row::Kind;
    nxui::App& app = c.app;
    switch (r.kind) {
        case K::Heading:
            end_line(c);
            if (ImGui::GetCursorPosY() > app.S(24)) ImGui::Dummy(ImVec2(0, app.S(8)));
            ImGui::SetCursorPosX(c.x0);
            nxui::heading(r.text.c_str());
            return;
        case K::Hint: {
            end_line(c);
            const float x = r.full_width ? c.x0 : c.x0 + app.S(kLabelW);
            ImGui::SetCursorPosX(x);
            nxui::hint(r.text, c.W - (x - c.x0));
            return;
        }
        case K::Gap:
            end_line(c);
            ImGui::Dummy(ImVec2(0, app.S(static_cast<float>(r.gap_px))));
            return;
        case K::Custom:
            end_line(c);
            ImGui::SetCursorPosX(c.x0);
            if (r.draw) r.draw(c.W);
            return;
        default:
            break;
    }

    // a control: it sits in a cell of the grid
    const Cell k = cell_of(c, r);
    if (!r.joined) {
        if (r.cols > 1 && r.col > 0 && c.in_line) {
            ImGui::SetCursorPosY(c.line_y);
        } else {
            end_line(c);
            if (r.cols > 1) {
                c.in_line = true;
                c.line_y = ImGui::GetCursorPosY();
                c.line_h = 0;
            }
        }
    }
    const float y = ImGui::GetCursorPosY();
    ImGui::PushID(&r);
    ImGui::BeginDisabled(!r.enabled);
    switch (r.kind) {
        case K::Check: {
            ImGui::SetCursorPos(ImVec2(r.cols == 1 ? k.control_x() : k.x, y));
            bool v = r.value_ != 0;
            if (ImGui::Checkbox(r.label.c_str(), &v)) {
                r.value_ = v ? 1 : 0;
                changed(c, r);
            }
            break;
        }
        case K::Combo: {
            label_text(r.label, k.x, y);
            ImGui::SetCursorPos(ImVec2(k.control_x(), y));
            ImGui::SetNextItemWidth(k.control_w());
            const char* preview = r.value_ >= 0 && r.value_ < static_cast<int>(r.items.size()) ? r.items[r.value_].c_str() : "";
            if (ImGui::BeginCombo("##c", preview)) {
                for (int i = 0; i < static_cast<int>(r.items.size()); ++i) {
                    ImGui::PushID(i);
                    if (ImGui::Selectable(r.items[i].c_str(), i == r.value_)) {
                        if (i != r.value_) {
                            r.value_ = i;
                            changed(c, r);
                        }
                    }
                    if (i == r.value_) ImGui::SetItemDefaultFocus();
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            break;
        }
        case K::Slider: {
            label_text(r.label, k.x, y);
            const float value_w = (r.compact && r.cols > 1) ? app.S(44) : app.S(100);
            ImGui::SetCursorPos(ImVec2(k.control_x(), y));
            ImGui::SetNextItemWidth(std::max(app.S(40), k.control_w() - value_w));
            int v = r.value_;
            if (ImGui::SliderInt("##s", &v, r.lo, r.hi, "", ImGuiSliderFlags_AlwaysClamp)) {
                r.value_ = v;
                changed(c, r);
            }
            ImGui::SameLine(0, app.S(8));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r.format ? r.format(r.value_).c_str() : std::to_string(r.value_).c_str());
            break;
        }
        case K::Number: {
            if (r.joined) {
                ImGui::SameLine(0, app.S(6));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("x");
                ImGui::SameLine(0, app.S(6));
            } else {
                label_text(r.label, k.x, y);
                ImGui::SetCursorPos(ImVec2(k.control_x(), y));
            }
            ImGui::SetNextItemWidth(app.S(static_cast<float>(r.width)));
            if (ImGui::InputText("##n", &r.number_buf, ImGuiInputTextFlags_CharsDecimal)) {
                if (r.number_buf.size() > 6) r.number_buf.resize(6);
                r.value_ = parse_int(r.number_buf);
                changed(c, r);
            }
            if (!r.text.empty() && r.joined) {
                ImGui::SameLine(0, app.S(8));
                ImGui::AlignTextToFramePadding();
                nxui::colored_text(nxui::col::gray, r.text);
            }
            break;
        }
        case K::Button: {
            const float x = (r.cols == 1 && !r.at_left) ? k.control_x() : k.x;
            ImGui::SetCursorPos(ImVec2(x, y));
            if (nxui::button(r.text.c_str(), ImVec2(app.S(static_cast<float>(r.width)), 0))) c.page.command(r.button_id);
            break;
        }
        case K::Text:
            label_text(r.label, k.x, y);
            ImGui::SetCursorPos(ImVec2(k.control_x(), y));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r.text.c_str());
            break;
        case K::Swatch: {
            label_text(r.label, k.x, y);
            ImGui::SetCursorPos(ImVec2(k.control_x(), y));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = std::min(k.control_w(), app.S(160));
            const float h = app.S(static_cast<float>(r.width));
            const uint32_t rgb = r.rgb ? r.rgb() : 0xFFFFFF;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255));
            dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_Border));
            ImGui::Dummy(ImVec2(w, h));
            break;
        }
        default:
            break;
    }
    ImGui::EndDisabled();
    mark_flash(r);
    ImGui::PopID();

    if (r.cols > 1 && c.in_line) {   // the next cell of the line starts at the same height
        c.line_h = std::max(c.line_h, ImGui::GetCursorPosY() - c.line_y);
        ImGui::SetCursorPosY(c.line_y);
    }
}

}  // namespace

void draw_page(Page& page, nxui::App& app, Host& host, float reserve_bottom) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(app.S(kMargin), app.S(12)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(app.S(8), app.S(kRowGap)));
    if (ImGui::BeginChild("##page", ImVec2(0, -reserve_bottom), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None)) {
        Ctx c{app, page, host};
        c.x0 = ImGui::GetCursorPosX();
        c.W = ImGui::GetContentRegionAvail().x;
        for (auto& r : page.rows()) {
            if (!r->visible) continue;
            draw_row(c, *r);
        }
        end_line(c);
        ImGui::Dummy(ImVec2(0, app.S(4)));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

}  // namespace nxl
