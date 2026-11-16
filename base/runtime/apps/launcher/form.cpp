// SPDX-License-Identifier: GPL-3.0-or-later
// The data side of the launcher's page API (nxrt/game/launcher_ui.h): what Form records and how a page moves its rows to and from Settings.
// No drawing here (page_view.cpp) and no window system, so the tests can run it alone.
#include <algorithm>
#include <string>
#include <utility>

#include "nxrt/game/launcher_ui.h"

namespace nxl {

Page::~Page() = default;

namespace {
int parse_int(const std::string& s) {
    long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') continue;
        v = v * 10 + (c - '0');
        if (v > 999999) break;
    }
    return static_cast<int>(v);
}
}  // namespace

void Page::load(const Settings& s) {
    for (auto& r : rows_) {
        if (!r->load) continue;
        r->load(s);
        if (r->kind == Row::Kind::Number) r->number_buf = std::to_string(r->value_);
    }
}

void Page::store(Settings* s) {
    for (auto& r : rows_) {
        if (!r->store) continue;
        if (r->kind == Row::Kind::Number) r->value_ = parse_int(r->number_buf);
        r->store(*s);
    }
}

Row* Page::row_after(const Row* r) {
    for (size_t i = 0; i + 1 < rows_.size(); ++i)
        if (rows_[i].get() == r) return rows_[i + 1].get();
    return nullptr;
}

// ---- Form -------------------------------------------------------------------------------------------------------------------------------

Row* Form::add(Row::Kind k) {
    page_.rows_.push_back(std::make_unique<Row>());
    Row* r = page_.rows_.back().get();
    r->kind = k;
    return r;
}

void Form::place(Row* r) {
    r->cols = cols_;
    r->col = col_;
    r->compact = compact_;
    if (cols_ > 1 && ++col_ >= cols_) col_ = 0;
}

void Form::columns(int n) {
    col_ = 0;   // a row left half filled ends where it is
    cols_ = std::max(1, std::min(n, 3));
}

void Form::heading(std::string text) {
    columns(1);
    Row* r = add(Row::Kind::Heading);
    r->text = std::move(text);
}

Row* Form::hint(std::string text, bool full_width) {
    columns(1);
    Row* r = add(Row::Kind::Hint);
    r->text = std::move(text);
    r->full_width = full_width;
    return r;
}

void Form::gap(int px) {
    Row* r = add(Row::Kind::Gap);
    r->gap_px = px;
}

Row* Form::checkbox(std::string text, GetBool get, SetBool set, After after) {
    Row* r = add(Row::Kind::Check);
    r->label = std::move(text);
    r->load = [r, get](const Settings& s) { r->value_ = get(s) ? 1 : 0; };
    r->store = [r, set](Settings& s) { set(s, r->value_ != 0); };
    r->after = std::move(after);
    place(r);
    return r;
}

Row* Form::combo(std::string label, std::vector<std::string> items, GetInt get, SetInt set, After after) {
    Row* r = add(Row::Kind::Combo);
    r->label = std::move(label);
    r->items = std::move(items);
    r->value_ = -1;
    r->load = [r, get](const Settings& s) { r->value_ = get(s); };
    r->store = [r, set](Settings& s) {
        if (r->value_ >= 0) set(s, r->value_);
    };
    r->after = std::move(after);
    place(r);
    return r;
}

Row* Form::slider(std::string label, int lo, int hi, GetInt get, SetInt set, std::function<std::string(int)> format, After after) {
    Row* r = add(Row::Kind::Slider);
    r->label = std::move(label);
    r->lo = lo;
    r->hi = hi;
    r->value_ = lo;
    r->load = [r, get, lo, hi](const Settings& s) { r->value_ = std::clamp(get(s), lo, hi); };
    r->store = [r, set](Settings& s) { set(s, r->value_); };
    r->format = std::move(format);
    r->after = std::move(after);
    place(r);
    return r;
}

Row* Form::number(std::string label, int lo, int hi, GetInt get, SetInt set, After after, int width) {
    Row* r = add(Row::Kind::Number);
    r->label = std::move(label);
    r->lo = lo;
    r->hi = hi;
    r->width = width;
    r->load = [r, get](const Settings& s) { r->value_ = get(s); };
    r->store = [r, set, lo, hi](Settings& s) { set(s, std::clamp(r->value_, lo, hi)); };
    r->after = std::move(after);
    place(r);
    return r;
}

Row* Form::number_pair(std::string label, int lo, int hi, GetInt get_a, SetInt set_a, GetInt get_b, SetInt set_b, After after, std::string suffix) {
    Row* a = number(std::move(label), lo, hi, std::move(get_a), std::move(set_a), after, 80);
    const int col_after_a = col_;
    Row* b = number("", lo, hi, std::move(get_b), std::move(set_b), std::move(after), 80);
    b->joined = true;
    b->text = std::move(suffix);
    b->cols = a->cols;
    b->col = a->col;   // the same cell
    col_ = col_after_a;
    return a;
}

int Form::button(std::string label, int width, bool at_left) {
    Row* r = add(Row::Kind::Button);
    r->text = std::move(label);
    r->width = width;
    r->at_left = at_left;
    r->button_id = next_button_++;
    place(r);
    return r->button_id;
}

Row* Form::text_row(std::string label, std::string text) {
    Row* r = add(Row::Kind::Text);
    r->label = std::move(label);
    r->text = std::move(text);
    place(r);
    return r;
}

Row* Form::swatch(std::string label, std::function<uint32_t()> rgb, int height_px) {
    Row* r = add(Row::Kind::Swatch);
    r->label = std::move(label);
    r->rgb = std::move(rgb);
    r->width = height_px;
    place(r);
    return r;
}

Row* Form::custom(std::function<void(float)> draw) {
    columns(1);
    Row* r = add(Row::Kind::Custom);
    r->draw = std::move(draw);
    return r;
}

}  // namespace nxl
