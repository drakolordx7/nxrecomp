// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's page API (nxrt/game/launcher_ui.h, apps/launcher/form.cpp): how a Form's rows move values from Settings to the rows and back,
// the grid the rows sit in, and the number pair. No window and no drawing.
#include "test_framework.h"

#include <memory>
#include <string>

#include "nxrt/game/launcher_ui.h"

NXTEST_GROUP(launcher_form)

namespace {

using nxl::Form;
using nxl::Page;
using nxl::Row;
using nxl::Settings;

class TestPage : public Page {
public:
    const char* title() const override { return "Test"; }
    void build(Form& f) override {
        check = f.checkbox("VSync", [](const Settings& s) { return s.display.vsync; }, [](Settings& s, bool v) { s.display.vsync = v; });
        combo = f.combo("Mode", {"a", "b", "c"}, [](const Settings& s) { return s.display.fps_cap == 60 ? 2 : -1; },
                        [](Settings& s, int i) { s.display.fps_cap = i * 10; });
        slider = f.slider("FOV", -30, 40, [](const Settings& s) { return static_cast<int>(s.display.fov_offset); },
                          [](Settings& s, int v) { s.display.fov_offset = v; }, [](int v) { return std::to_string(v); });
        pair = f.number_pair("Size", 320, 16384, [](const Settings& s) { return s.display.width; }, [](Settings& s, int v) { s.display.width = v; },
                             [](const Settings& s) { return s.display.height; }, [](Settings& s, int v) { s.display.height = v; }, nullptr, "pixels");
        f.columns(2);
        left = f.text_row("L", "x");
        right = f.text_row("R", "y");
        third = f.text_row("T", "z");
        button_id = f.button("Go", 100);
    }
    Row *check = nullptr, *combo = nullptr, *slider = nullptr, *pair = nullptr, *left = nullptr, *right = nullptr, *third = nullptr;
    int button_id = 0;
};

std::unique_ptr<TestPage> make_page() {
    auto p = std::make_unique<TestPage>();
    Form f(*p);
    p->build(f);
    return p;
}

}  // namespace

NXTEST(rows_take_their_values_from_settings) {
    auto p = make_page();
    Settings s;
    s.display.vsync = true;
    s.display.fps_cap = 60;
    s.display.fov_offset = 12;
    s.display.width = 1920;
    s.display.height = 1080;
    p->load(s);
    CHECK_EQ(p->check->value(), 1);
    CHECK_EQ(p->combo->value(), 2);
    CHECK_EQ(p->slider->value(), 12);
    CHECK_EQ(p->pair->value(), 1920);
    CHECK_EQ(p->row_after(p->pair)->value(), 1080);
    CHECK(p->pair->number_buf == "1920");
}

NXTEST(store_writes_what_the_player_changed_and_clamps_numbers) {
    auto p = make_page();
    Settings s;
    p->load(s);
    p->check->value_ = 1;
    p->slider->value_ = -7;
    p->pair->number_buf = "99";      // typed, below the lower limit
    p->row_after(p->pair)->number_buf = "20000";
    p->combo->value_ = 1;
    Settings out = s;
    p->store(&out);
    CHECK(out.display.vsync);
    CHECK_EQ(static_cast<int>(out.display.fov_offset), -7);
    CHECK_EQ(out.display.width, 320);
    CHECK_EQ(out.display.height, 16384);
    CHECK_EQ(out.display.fps_cap, 10);
}

NXTEST(a_combo_with_no_item_chosen_writes_nothing) {
    auto p = make_page();
    Settings s;
    s.display.fps_cap = 144;
    p->load(s);
    CHECK_EQ(p->combo->value(), -1);
    Settings out = s;
    p->store(&out);
    CHECK_EQ(out.display.fps_cap, 144);
}

NXTEST(columns_fill_left_to_right_and_wrap) {
    auto p = make_page();
    CHECK_EQ(p->left->cols, 2);
    CHECK_EQ(p->left->col, 0);
    CHECK_EQ(p->right->col, 1);
    CHECK_EQ(p->third->col, 0);   // wrapped to the next line
    // the number pair sits in one cell
    CHECK(p->row_after(p->pair)->joined);
    CHECK_EQ(p->row_after(p->pair)->col, p->pair->col);
}

NXTEST(buttons_get_their_own_ids) {
    auto p = make_page();
    CHECK(p->button_id > 0);
}
