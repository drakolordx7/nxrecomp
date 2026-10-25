// SPDX-License-Identifier: GPL-3.0-or-later
// The game this setup tool is built for: which dump it accepts and what the files it writes are called. One definition per game in
// src/games/<id>.cpp, picked by the CMake cache variable MPRS_GAME (runtime/docs/GAME_BOUNDARY.md). The rest of the tool reads these
// values instead of naming a title.
#pragma once

namespace mprs {

struct GameProfile {
    const char* id;            // "mpr"
    const char* title;         // the game's name in messages: "Metroid Prime Remastered"
    const char* product;       // the port's name in window titles: "Metroid Prime Remastered PC"
    const char* title_id;      // the program's title id, 16 hex digits, upper case
    const char* build_id;      // main NSO build id, hex, first 20 bytes: the one supported release
    const char* version;       // that release's display version: "1.0.0"
    const char* game_ini;      // the runtime's settings file setup writes into the game folder: "mpr.ini"
    const char* shader_pack;   // the shader pack next to the setup program: "shader_cache.mprshc"
};

const GameProfile& game();     // src/games/<MPRS_GAME>.cpp

}  // namespace mprs
