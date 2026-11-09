// SPDX-License-Identifier: GPL-3.0-or-later
// The game this setup tool is built for: which dump it accepts and what the files it writes are called. Generated from the game's
// manifest games/<id>/game.json (src/game_profile.cpp; the CMake cache variable NX_GAME_DIR names the game folder). The rest of the tool
// reads these values instead of naming a title.
#pragma once

namespace mprs {

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
    const char* build_id;      // main NSO build id, hex, first 20 bytes: the one supported release
    const char* version;       // that release's display version: "1.0.0"
    const char* game_ini;      // the runtime's settings file setup writes into the game folder: "<id>.ini"
    const char* shader_pack;   // the shader pack next to the setup program: "shader_cache.mprshc"
    // Optional: every supported release (base or base + update), ending with {nullptr, nullptr}. When null or empty, the single
    // build_id / version above is the one supported release.
    const GameBuild* builds = nullptr;
};

const GameProfile& game();     // src/game_profile.cpp

// True when the profile lists its supported releases (GameProfile::builds); such a game may need an update.
inline bool game_lists_builds() { return game().builds && game().builds[0].build_id; }

}  // namespace mprs
