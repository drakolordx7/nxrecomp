// SPDX-License-Identifier: GPL-3.0-or-later
// The profile of the game this setup tool is built for (game.h), from that game's manifest games/<id>/game.json: CMake generates
// nx_game.h from it (cmake/nx_game.cmake; NX_GAME_DIR names the game folder). Edit the manifest, not this file.
#include "game.h"
#include "nx_game.h"

namespace mprs {

namespace {
const GameBuild kBuilds[] = {
    NXGAME_BUILDS
    {nullptr, nullptr},
};
}  // namespace

const GameProfile& game() {
    // The single build_id / version are those of the newest listed build (the last row of the manifest's builds).
    static const GameProfile g{
        NXGAME_ID, NXGAME_TITLE, NXGAME_PRODUCT, NXGAME_TITLE_ID, kBuilds[sizeof(kBuilds) / sizeof(kBuilds[0]) - 2].build_id, NXGAME_VERSION,
        NXGAME_GAME_INI, NXGAME_SHADER_PACK, kBuilds,
    };
    return g;
}

}  // namespace mprs
