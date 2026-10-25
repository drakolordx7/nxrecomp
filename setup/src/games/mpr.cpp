// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered 1.0.0 (game.h). The build id is that of main (recomp/games/mpr/config.py).
#include "game.h"

namespace mprs {

const GameProfile& game() {
    static const GameProfile g{
        "mpr",
        "Metroid Prime Remastered",
        "Metroid Prime Remastered PC",
        "010012101468C000",
        "67C9CA1428222752F951021464A7B3597EB98FEC",
        "1.0.0",
        "mpr.ini",
        "shader_cache.mprshc",
    };
    return g;
}

}  // namespace mprs
