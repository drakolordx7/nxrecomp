// SPDX-License-Identifier: GPL-3.0-or-later
// The [tweaks] section of controls.ini: reader and writer (mpr_settings.h).
#include "mpr_settings.h"

namespace nxrt::play {

namespace {

bool apply(GameState& st, const std::string& key, const std::string& value, bool* known, std::string* msg) {
    MprSettings& m = st.get<MprSettings>();
    bool mine = false;
    if (!fastboot_apply(key, value, &m.fastboot, &mine, msg)) return false;
    if (mine) {
        *known = true;
        return true;
    }
    return apply_tweaks_key(key, value, &m.tweaks, known, msg);
}

void format(const GameState& st, std::string* out) {
    const MprSettings& m = st.get<MprSettings>();
    fastboot_format(m.fastboot, out);
    *out += format_tweaks(m.tweaks);
}

}

const GameSection& mpr_settings_section_data() {
    static const GameSection s = [] {
        GameSection g;
        g.name = "tweaks";
        g.apply = &apply;
        g.format = &format;
        return g;
    }();
    return s;
}

}
