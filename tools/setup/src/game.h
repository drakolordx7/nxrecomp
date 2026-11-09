// SPDX-License-Identifier: GPL-3.0-or-later
// The game this setup tool is built for: which dump it accepts and what the files it writes are called. Generated from the game's
// manifest games/<id>/game.json (src/game_profile.cpp; the CMake cache variable NX_GAME_DIR names the game folder). The rest of the tool
// reads these values instead of naming a title.
#pragma once
#include <vector>

namespace nxs {

// One release setup accepts: the main NSO build id (hex, first 20 bytes; shorter ids are padded with zeros) and its display version.
struct GameBuild {
    const char* build_id;
    const char* version;
};

struct GameProfile {
    const char* id;            // "<id>", the manifest id
    const char* title;         // the game's name in messages (title)
    const char* product;       // the port's name in window titles (product)
    const char* title_id;      // the program's title id, 16 hex digits, upper case
    const char* game_ini;      // the runtime's settings file setup writes into the game folder: "<id>.ini"
    const char* shader_pack;   // the shader pack next to the setup program, "" when the game ships none
    std::vector<GameBuild> builds;   // every release setup accepts (base or base + update), oldest first; the manifest's builds[]
};

const GameProfile& game();     // src/game_profile.cpp: the profile of the game this tool is built for

// The version text of a profile's newest build ("" when it lists none).
inline const char* newest_version(const GameProfile& g) { return g.builds.empty() || !g.builds.back().version ? "" : g.builds.back().version; }

}  // namespace nxs
