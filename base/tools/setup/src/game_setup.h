// SPDX-License-Identifier: GPL-3.0-or-later
// The game hook of the setup tool. The base installer (keys, containers, NCA, update patches, ExeFS/RomFS/DLC extraction, the shader pack,
// the state file, the ini) knows nothing about how one game stores its data; a game that needs more than extraction supplies a GameSetup:
//
//   ini_lines       extra lines for the game ini, after the base's [fs] and [nvn] sections
//   texture_cache   the game's ASTC textures, for the BC7 cache writer (texcache.h). Optional: without it there is no Textures stage.
//   program_list    every shader program found in the game's files, for shader_cache/programs.bin (nx_formats.h). Optional.
//   disk_estimate   what the stages above write, for the free-space check
//
// The hook lives in the game folder (games/<id>/setup/, named by the manifest's "setup_hook" CMake file) and is linked in at build time:
// the setup tool is built for one game (NX_GAME_DIR). A game with no hook gets game_setup() == nullptr and the stages are skipped.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "common.h"
#include "nx_formats.h"
#include "texcache.h"

namespace nxs {

struct DiskEstimate {
    u64 texture_cache = 0;   // bytes of bc7_cache for the whole game
    u64 shader_cache = 0;    // bytes of shader_cache/spirv; 0 = the size of the shader pack file
};

struct TextureList {
    std::vector<AstcTexture> textures;
    u32 failed = 0;          // textures the hook found but could not describe (logged by the hook; reported as failed)
};

struct ProgramList {
    std::vector<nxfmt::ProgramHashes> programs;   // sorted ascending, unique
    std::string summary;                          // one line for the log: where they came from
};

struct GameSetup {
    std::function<std::vector<std::string>()> ini_lines;
    // romfs_dir: the extracted RomFS. cancelled() is polled; throw Error(Fail::Cancelled) to stop.
    std::function<TextureList(const std::string& romfs_dir, const std::function<bool()>& cancelled)> texture_cache;
    // threads: 0 = all hardware threads. A failure throws; setup logs it and goes on without a program list.
    std::function<ProgramList(const std::string& romfs_dir, int threads, const std::function<bool()>& cancelled)> program_list;
    std::function<DiskEstimate()> disk_estimate;
};

// The hook of the game this tool is built for (the game's own source file), or nullptr when it has none (src/game_setup_none.cpp).
const GameSetup* game_setup();

}  // namespace nxs
