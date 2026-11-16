// SPDX-License-Identifier: GPL-3.0-or-later
// The player-facing name of an input ("Left Ctrl", "Left mouse button", "Mouse wheel up"). Header only: the names are the play settings' own.
#pragma once
#include <string>

#include "nxrt/game/play_types.h"

namespace nxl {

inline std::string input_label(const nxrt::play::InputId& id) {
    using nxrt::play::InKind;
    switch (id.kind) {
        case InKind::Mouse: {
            static const char* const names[] = {"", "Left mouse button", "Middle mouse button", "Right mouse button", "Mouse button 4", "Mouse button 5"};
            return id.code >= 1 && id.code <= 5 ? names[id.code] : "Mouse button";
        }
        case InKind::Wheel: return id.code > 0 ? "Mouse wheel up" : "Mouse wheel down";
        case InKind::Key: {
            const std::string n = nxrt::play::input_name(id);
            return n.empty() ? "?" : n;
        }
        default: return "";
    }
}

}  // namespace nxl
