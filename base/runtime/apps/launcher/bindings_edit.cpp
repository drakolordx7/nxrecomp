// SPDX-License-Identifier: GPL-3.0-or-later
// Keyboard tab, logic part (see bindings_edit.h).
#include "bindings_edit.h"

#include <cstddef>
#include <string>
#include <utility>

#include "game_info.h"

namespace nxl {

using nxrt::play::Act;
using nxrt::play::Binding;
using nxrt::play::Ctx;
using nxrt::play::InputId;
using nxrt::play::Settings;
using nxrt::play::Trig;

// The registered game's rows (nxrt::play::GameVocabulary::labels). Without a game: the
// movement, menu and d-pad rows under generic names.
const std::vector<KeyRow>& key_rows() {
    static std::vector<std::string> text;
    static const std::vector<KeyRow> rows = [] {
        std::vector<KeyRow> r;
        const nxrt::play::GameVocabulary* v = nxrt::play::game_vocabulary();
        if (v && v->labels && v->label_count) {
            text.reserve(2 * v->label_count);
            for (unsigned i = 0; i < v->label_count; ++i) {
                text.push_back(game_text(v->labels[i].label, ""));
                text.push_back(game_text(v->labels[i].group, ""));
                r.push_back({v->labels[i].act, text[text.size() - 2].c_str(), text.back().c_str()});
            }
            return r;
        }
        return std::vector<KeyRow>{
            {Act::MoveForward, "Move forward", "Movement"}, {Act::MoveBack, "Move back", "Movement"},
            {Act::MoveLeft, "Move left", "Movement"},       {Act::MoveRight, "Move right", "Movement"},
            {Act::Map, "Map", "Menus"},                     {Act::Pause, "Pause", "Menus"},
            {Act::DpadUp, "D-pad up", "Menus"},             {Act::DpadDown, "D-pad down", "Menus"},
            {Act::DpadLeft, "D-pad left", "Menus"},         {Act::DpadRight, "D-pad right", "Menus"},
        };
    }();
    return rows;
}

namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

std::vector<std::size_t> own_indexes(const Settings& s, Act a) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < s.bindings.size(); ++i)
        if (s.bindings[i].ctx == Ctx::All && s.bindings[i].act == a) out.push_back(i);
    return out;
}

void erase_marked(std::vector<Binding>& v, const std::vector<bool>& drop) {
    std::size_t w = 0;
    for (std::size_t r = 0; r < v.size(); ++r) {
        if (drop[r]) continue;
        if (w != r) v[w] = v[r];
        ++w;
    }
    v.resize(w);
}

}  // namespace

std::vector<InputId> inputs_for(const Settings& s, Act a) {
    std::vector<InputId> out;
    for (const Binding& b : s.bindings)
        if (b.ctx == Ctx::All && b.act == a) out.push_back(b.in);
    return out;
}

void bind_slot(Settings& s, Act a, int slot, const InputId& in_arg, Act* displaced) {
    const InputId in = in_arg;
    if (displaced) *displaced = Act::None;

    std::vector<Binding>& v = s.bindings;
    const std::vector<std::size_t> own = own_indexes(s, a);
    if (slot != 1) slot = 0;
    if (slot == 1 && own.empty()) slot = 0;

    std::vector<bool> drop(v.size(), false);

    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].ctx != Ctx::All || v[i].act == a || !(v[i].in == in)) continue;
        drop[i] = true;
        if (displaced && *displaced == Act::None) *displaced = v[i].act;
    }

    const std::size_t target = slot < static_cast<int>(own.size()) ? own[static_cast<std::size_t>(slot)] : kNone;
    for (std::size_t j : own) {
        if (j != target && v[j].in == in) drop[j] = true;
    }
    if (target != kNone) v[target].in = in;

    erase_marked(v, drop);
    if (target == kNone) v.push_back(Binding{in, Ctx::All, a, Trig::Press, 0});
}

void clear_slot(Settings& s, Act a, int slot) {
    const std::vector<std::size_t> own = own_indexes(s, a);
    if (slot < 0 || slot >= static_cast<int>(own.size())) return;
    s.bindings.erase(s.bindings.begin() + static_cast<std::ptrdiff_t>(own[static_cast<std::size_t>(slot)]));
}

void reset_shown_bindings(Settings& s) {
    const Settings factory = nxrt::play::default_settings();
    std::vector<Binding> out;
    out.reserve(s.bindings.size() + factory.bindings.size());
    bool placed = false;
    for (const Binding& b : s.bindings) {
        if (b.ctx != Ctx::All) {
            out.push_back(b);
            continue;
        }
        if (placed) continue;
        for (const Binding& f : factory.bindings)
            if (f.ctx == Ctx::All) out.push_back(f);
        placed = true;
    }
    if (!placed) {
        std::vector<Binding> all;
        for (const Binding& f : factory.bindings)
            if (f.ctx == Ctx::All) all.push_back(f);
        out.insert(out.begin(), all.begin(), all.end());
    }
    s.bindings = std::move(out);
}

std::string act_label(Act a) {
    for (const KeyRow& r : key_rows())
        if (r.act == a) return r.label;
    return "";
}

}  // namespace nxl
