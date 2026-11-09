// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/game/spirv_module.h"

namespace nxrt::spirv {

bool split(const uint32_t* words, size_t count, std::vector<Inst>& out) {
    out.clear();
    for (size_t i = kHeaderWords; i < count;) {
        const uint32_t len = words[i] >> 16;
        if (!len || i + len > count) return false;
        out.push_back({i, words[i] & 0xffff, len});
        i += len;
    }
    return true;
}

std::string literal_string(const uint32_t* w, size_t n, size_t* used) {
    std::string s;
    size_t i = 0;
    for (; i < n; i++) {
        bool end = false;
        for (int b = 0; b < 4; b++) {
            char ch = char((w[i] >> (8 * b)) & 0xff);
            if (!ch) {
                end = true;
                break;
            }
            s.push_back(ch);
        }
        if (end) {
            i++;
            break;
        }
    }
    if (used) *used = i;
    return s;
}

}  // namespace nxrt::spirv
