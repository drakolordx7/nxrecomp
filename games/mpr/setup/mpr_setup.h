// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's part of the setup tool: the textures and shader programs of its Retro .pak archives, handed to the base
// installer through the game hook (base tools/setup/src/game_setup.h).
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "game_setup.h"
#include "pak.h"
#include "txtr.h"

namespace nxs::mpr {

// The ASTC textures of every .pak under the extracted RomFS (reads the TXTR headers only; the buffers are read when the cache writer asks).
TextureList list_textures(const std::string& romfs_dir, const std::function<bool()>& cancelled);

// One TXTR as the generic cache writer's description. read_tiled reads its tiled buffer (the pak overload keeps the pak alive).
AstcTexture astc_texture_of(const TxtrInfo& info, std::function<std::vector<u8>()> read_tiled);
AstcTexture astc_texture_of(const TxtrInfo& info, std::shared_ptr<const Pak> pak, const PakEntry& entry);

// Every shader program of MaterialArchive.arc and the .pak files under the RomFS (programs.h).
ProgramList list_programs(const std::string& romfs_dir, int threads, const std::function<bool()>& cancelled);

}  // namespace nxs::mpr
