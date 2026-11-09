// SPDX-License-Identifier: GPL-3.0-or-later
// Front ends of the test tooling: the start keys players and tools used before the scripted-run language was the one language, turned into
// script text. See tt.h (press_list_to_script, set_script_text).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "nxrt/tt.h"

namespace nxrt::tt {

namespace {
bool known_button(const std::string& n) {
    static const char* const names[] = {"A", "B", "X", "Y", "PLUS", "MINUS", "UP", "DOWN", "LEFT", "RIGHT", "L", "R", "ZL", "ZR"};
    for (const char* k : names) if (n == k) return true;
    return false;
}
}  // namespace

std::string press_list_to_script(const std::string& spec, std::string*) {
    std::string out;
    double cursor_ms = 0;                 // where the script is, in milliseconds since its start, as far as the seconds-keyed presses know
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        if (j == std::string::npos) j = spec.size();
        const std::string item = spec.substr(i, j - i);
        i = j + 1;
        size_t at = item.find('@');
        const bool by_frame = at == std::string::npos && item.find('#') != std::string::npos;
        if (by_frame) at = item.find('#');
        if (at == std::string::npos) continue;
        const std::string name = item.substr(0, at);
        if (!known_button(name)) continue;
        const double when = std::atof(item.c_str() + at + 1);
        const size_t colon = item.find(':', at);
        const int hold_ms = colon == std::string::npos ? 300 : std::atoi(item.c_str() + colon + 1);
        char b[96];
        if (by_frame) {
            std::snprintf(b, sizeof b, "@frame %llu\n", static_cast<unsigned long long>(when));
        } else {
            std::snprintf(b, sizeof b, "@sleep %.0f\n", std::max(0.0, when * 1000.0 - cursor_ms));
            cursor_ms = std::max(cursor_ms, when * 1000.0) + hold_ms;
        }
        out += b;
        out += "hold " + name + "\n";
        std::snprintf(b, sizeof b, "@sleep %d\n", hold_ms);
        out += b;
        out += "release " + name + "\n";
    }
    return out;
}

}  // namespace nxrt::tt
