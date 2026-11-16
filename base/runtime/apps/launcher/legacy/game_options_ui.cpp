// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher rows of the options a game registered (nxrt/game/settings.h GameOption): one check box, drop-down or slider per row, built from
// the table. A slider only writes its value when the player moved it, so a value in the settings file with more digits than the slider can
// show stays as it is.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "game_info.h"
#include "nxrt/game.h"
#include "pages.h"

namespace nxl {

using nxrt::play::GameOption;
using nxrt::play::GameSection;
using nxrt::play::OptKind;
using nxrt::play::OptTab;

namespace {

double pow10i(int n) {
    double v = 1;
    for (int i = 0; i < n; ++i) v *= 10;
    return v;
}

int slider_pos(const GameOption& o, double stored) {
    const double k = pow10i(o.ui_decimals);
    return static_cast<int>(std::clamp(std::llround(stored * o.ui_scale * k), std::llround(o.ui_lo * k), std::llround(o.ui_hi * k)));
}

std::wstring pos_text(const GameOption& o, int pos) {
    const double k = pow10i(o.ui_decimals);
    std::wstring t;
    if (o.ui_decimals == 0) {
        t = std::to_wstring(pos);
    } else {
        const long long whole = static_cast<long long>(k);
        t = std::to_wstring(pos / whole) + L"." + std::to_wstring(pos % whole);
    }
    return t + game_text(o.ui_unit, L"");
}

}  // namespace

void GameOptionRows::build(Form& f, OptTab tab) {
    const GameSection* gs = nxrt::game::info().settings;
    if (!gs) return;
    const char* group = nullptr;
    for (unsigned i = 0; i < gs->option_count; ++i) {
        const GameOption& o = gs->options[i];
        if (!(o.ui & nxrt::play::kOptLauncher) || o.tab != tab) continue;
        if (o.group && o.group != group) {
            group = o.group;
            texts_.push_back(game_text(o.group, L""));
            f.heading(texts_.back().c_str());
        }
        texts_.push_back(game_text(o.label, L""));
        const wchar_t* label = texts_.back().c_str();
        const GameOption* op = &o;
        const unsigned idx = i;
        orig_.resize(gs->option_count, 0);
        state_.resize(gs->option_count, 0);
        rows_.resize(gs->option_count);
        switch (o.kind) {
            case OptKind::Bool:
                f.checkbox(
                    label, [op](const Settings& s) { return (op->get(s.game) != 0) != op->inverted; },
                    [op](Settings& s, bool v) { op->set(s.game, (v != op->inverted) ? 1.0 : 0.0); }, [this, idx]() { toggled(idx); });
                break;
            case OptKind::Choice: {
                std::vector<std::wstring> items;
                for (unsigned w = 0; w < o.word_count; ++w) items.push_back(game_text(o.labels ? o.labels[w] : o.words[w], L""));
                rows_[idx] = f.combo(
                    label, items, [op](const Settings& s) { return static_cast<int>(op->get(s.game)); },
                    [op](Settings& s, int v) { op->set(s.game, v); }, [this, idx]() { toggled(idx); });
                break;
            }
            case OptKind::Real: {
                const double k = pow10i(o.ui_decimals);
                rows_[idx] = f.slider(
                    label, static_cast<int>(std::llround(o.ui_lo * k)), static_cast<int>(std::llround(o.ui_hi * k)),
                    [op](const Settings& s) { return slider_pos(*op, op->get(s.game)); },
                    [this, op, idx](Settings& s, int pos) {
                        if (pos == orig_[idx]) return;
                        op->set(s.game, pos / pow10i(op->ui_decimals) / op->ui_scale);
                    },
                    [op](int pos) { return pos_text(*op, pos); });
                break;
            }
        }
        if (o.hint) {
            texts_.push_back(game_text(o.hint, L""));
            f.hint(texts_.back().c_str(), 3);
        }
    }
}

void GameOptionRows::load(const Settings& s) {
    const GameSection* gs = nxrt::game::info().settings;
    for (unsigned i = 0; gs && i < gs->option_count && i < orig_.size(); ++i) {
        const GameOption& o = gs->options[i];
        const double v = o.get(s.game);
        if (o.kind == OptKind::Real) orig_[i] = slider_pos(o, v);
        state_[i] = v != 0;
    }
    refresh_enabled();
}

void GameOptionRows::toggled(unsigned idx) {
    const GameSection* gs = nxrt::game::info().settings;
    if (!gs) return;
    if (gs->options[idx].kind == OptKind::Choice && rows_[idx]) state_[idx] = SendMessageW(rows_[idx], CB_GETCURSEL, 0, 0) != 0;
    else state_[idx] = !state_[idx];
    refresh_enabled();
}

void GameOptionRows::refresh_enabled() {
    const GameSection* gs = nxrt::game::info().settings;
    for (unsigned i = 0; gs && i < gs->option_count && i < rows_.size(); ++i) {
        const GameOption& o = gs->options[i];
        if (!o.depends || !rows_[i]) continue;
        for (unsigned j = 0; j < gs->option_count; ++j)
            if (std::string(gs->options[j].key) == o.depends) EnableWindow(rows_[i], state_[j] != 0);
    }
}

}  // namespace nxl
