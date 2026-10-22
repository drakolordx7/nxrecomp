// SPDX-License-Identifier: GPL-3.0-or-later
// Retro "PACK" archives (the *.pak files of the RomFS) and the TXTR texture asset inside them. Only what the setup tool needs:
// list the assets, find the TXTR ones, read a texture's header and its tiled (block-linear) GPU buffer.
// Format knowledge: refs/retrotool (MIT) lib/src/format/pack.rs, txtr.rs, util/compression.rs and tools/survey/*.py of this
// project; re-implemented, no code copied.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "storage.h"

namespace mprs {

struct Uuid {
    u8 b[16] = {};                 // the 16 bytes as stored (little-endian GUID layout)
    std::string str() const;       // canonical "8-4-4-4-12" lower-case, same text as python uuid.UUID(bytes_le=b)
    bool operator==(const Uuid& o) const { for (int i = 0; i < 16; ++i) if (b[i] != o.b[i]) return false; return true; }
};

struct PakEntry {
    char type[5] = {};             // FourCC + NUL, e.g. "TXTR"
    Uuid id;
    u32 version = 0, other_version = 0;
    u64 offset = 0;                // absolute offset of the asset bytes in the pak file
    u64 decompressed_size = 0;
    u64 size = 0;                  // stored size; equal to decompressed_size when the asset is not compressed at pak level
};

class Pak {
public:
    // Reads RFRM PACK > TOCC: the ADIR directory and the META table. Throws Error(Input) if it is not a PACK or is malformed.
    static std::unique_ptr<Pak> open(StoragePtr file);
    const std::vector<PakEntry>& assets() const { return assets_; }
    // The asset's own bytes (an RFRM form) as stored. Throws Error(Input) if size != decompressed_size (pak-level compression is
    // not needed for TXTR in this game; the tool counts and reports such entries).
    std::vector<u8> read_asset(const PakEntry& e) const;
    // The per-asset META blob of the TOCC META chunk (empty if the asset has none).
    std::vector<u8> read_meta(const PakEntry& e) const;
    const StoragePtr& file() const { return file_; }
private:
    friend struct PakImpl;
    StoragePtr file_;
    std::vector<PakEntry> assets_;
    std::vector<u8> meta_chunk_;                         // raw body of the META chunk (empty if none)
    std::vector<std::pair<Uuid, u32>> meta_index_;       // asset id -> offset of its (u32 size, bytes) record inside meta_chunk_
};

}  // namespace mprs
