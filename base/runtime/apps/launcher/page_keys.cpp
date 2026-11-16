// SPDX-License-Identifier: GPL-3.0-or-later
// Keyboard tab: every action of the game with its two keys. Double-click a key to change it. Delete (or the clear button) removes
// the selected key. The menus keep their own keys. The list rows and the edits are in bindings_edit.cpp.
#include <imgui.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "bindings_edit.h"
#include "game_info.h"
#include "input_label.h"
#include "pages.h"

namespace nxl {

namespace {

std::string untouched_note() {
    const nxrt::play::GameVocabulary* v = nxrt::play::game_vocabulary();
    return v ? game_text(v->keys_note, "") : std::string();
}

using nxrt::play::Act;
using nxrt::play::InputId;
using nxrt::play::Settings;

constexpr float kButtonW = 190.f;   // logical px

class KeysPage : public Page {
public:
    const char* title() const override { return "Keyboard"; }

    void build(Form& f) override {
        f.custom([this](float width) { draw(width); });
    }

    void load(const Settings& s) override {
        edit_ = s;
        Page::load(s);
    }

    void store(Settings* s) override {
        Page::store(s);
        s->bindings = edit_.bindings;
    }

private:
    // Columns: 0 action, 1 first key, 2 second key.
    void draw(float width) {
        const float S = ImGui::GetFontSize() / 14.f;   // the UI scale (the base font size is 14 logical px)
        const std::vector<KeyRow>& rows = key_rows();
        const float list_h = ImGui::GetContentRegionAvail().y - 64.f * S;
        int act_row = -1, act_slot = 0;
        bool clear = false;
        if (ImGui::BeginTable("##keys", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(width, list_h > 120.f * S ? list_h : 120.f * S))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 4.f);
            ImGui::TableSetupColumn("Key 1", ImGuiTableColumnFlags_WidthStretch, 3.f);
            ImGui::TableSetupColumn("Key 2", ImGuiTableColumnFlags_WidthStretch, 3.f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                const KeyRow& r = rows[static_cast<std::size_t>(i)];
                const std::vector<InputId> keys = inputs_for(edit_, r.act);
                ImGui::TableNextRow();
                ImGui::PushID(i);
                for (int c = 0; c < 3; ++c) {
                    ImGui::TableSetColumnIndex(c);
                    std::string text = c == 0 ? std::string(r.group) + ": " + r.label
                                              : (static_cast<int>(keys.size()) > c - 1 ? input_label(keys[static_cast<std::size_t>(c - 1)]) : std::string());
                    const bool sel = sel_row_ == i && (c == 0 || sel_col_ == c);
                    ImGui::PushID(c);
                    if (ImGui::Selectable((text + "###cell").c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                        sel_row_ = i;
                        sel_col_ = c == 0 ? sel_col_ : c;
                        if (c == 0) sel_col_ = 0;
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            act_row = i;
                            act_slot = c == 2 ? 1 : 0;
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            if (sel_row_ >= 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Delete)) clear = true;
            ImGui::EndTable();
        }
        nxui_hint();
        if (ImGui::Button("Reset keys to defaults", ImVec2(kButtonW * S, 0))) reset_ = true;
        ImGui::SameLine();
        if (ImGui::Button("Clear selected key", ImVec2(kButtonW * S, 0))) clear = true;

        if (reset_) {
            reset_ = false;
            host()->ask("Reset the keys on this tab to the factory settings? Only the keys shown here are reset. " + untouched_note(), {"OK", "Cancel"},
                        [this](int a) {
                            if (a != 0) return;
                            reset_shown_bindings(edit_);
                            host()->changed();
                        });
        }
        if (clear && sel_row_ >= 0) clear_key(sel_row_);
        if (act_row >= 0) change_key(act_row, act_slot);
    }

    static void nxui_hint() {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.373f, 0.373f, 0.373f, 1.f));
        ImGui::TextUnformatted("Double-click a key to change it. Delete clears the selected key. Menus keep their own keys.");
        ImGui::PopStyleColor();
    }

    bool row_action(int row, Act* out) const {
        const std::vector<KeyRow>& rows = key_rows();
        if (row < 0 || row >= static_cast<int>(rows.size())) return false;
        *out = rows[static_cast<std::size_t>(row)].act;
        return true;
    }

    void change_key(int row, int slot) {
        Act act = Act::None;
        if (!row_action(row, &act)) return;
        host()->capture_input(act_label(act), [this, act, slot](bool got, InputId in) {
            if (!got) return;
            Act displaced = Act::None;
            bind_slot(edit_, act, slot, in, &displaced);
            host()->changed();
            if (displaced != Act::None)
                host()->message(input_label(in) + " was used for " + act_label(displaced) + "; that action now has one key less.");
        });
    }

    void clear_key(int row) {
        Act act = Act::None;
        if (!row_action(row, &act)) return;
        clear_slot(edit_, act, sel_col_ == 2 ? 1 : 0);
        host()->changed();
    }

    int sel_row_ = -1;
    int sel_col_ = 1;    // column of the last click: 2 is the second key, anything else the first
    bool reset_ = false;
    Settings edit_;      // the page's working copy; store() writes its bindings back
};

}  // namespace

std::unique_ptr<Page> make_keys_page() { return std::make_unique<KeysPage>(); }

}  // namespace nxl
