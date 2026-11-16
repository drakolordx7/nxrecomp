// SPDX-License-Identifier: GPL-3.0-or-later
// Small string helpers the runtime, the game exe and the launcher share. Header only. trim and lower work on ASCII (C locale); widen and narrow
// convert between UTF-8 and UTF-16 (Windows only: nxrt/os/wstr.h). The strict forms refuse invalid input.
#pragma once
#include <string>
#include <string_view>

#include "nxrt/os/wstr.h"       // widen / narrow (UTF-8 <-> UTF-16), Windows only

namespace nxrt::str {

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

inline std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

inline std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

inline std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return s;
}

}  // namespace nxrt::str
