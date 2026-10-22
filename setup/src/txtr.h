// SPDX-License-Identifier: GPL-3.0-or-later
// TXTR asset: header, format table, and the decompressed tiled GPU buffer. See pak.h for sources.
#pragma once
#include <vector>

#include "pak.h"

namespace mprs {

// LZSS as used by the TXTR buffers (modes 1..3; mode 0 = stored). Throws Error(Input) if the stream is malformed or does not
// produce exactly out_len bytes. `in` points at the 4-byte mode word followed by the data (the buffer exactly as stored).
std::vector<u8> txtr_decompress(const u8* in, size_t in_len, size_t out_len);

enum class TxtrKind : u32 { D1 = 0, D2, D3, Cube, D1Array, D2Array, D2MS, D2MSArray, CubeArray };

struct TxtrFormat {
    const char* name;      // "Astc5x4", "Bc7Unorm", "Rgba8Srgb", ... exactly the names of tools/survey/txtr_tally.py FORMATS
    u32 block_w, block_h;  // texels per block (1x1 for uncompressed)
    u32 bytes;             // bytes per block / per texel
    bool astc, bc, srgb;   // ASTC LDR family, BC1..BC7/BC6H family ("Bptc*" included), sRGB-encoded
};
// Index = the format id stored in the TXTR HEAD. Throws Error(Input) for an id outside the table; a table entry named "None"
// has bytes == 0.
const TxtrFormat& txtr_format(u32 id);

struct TxtrInfo {
    Uuid id;
    TxtrKind kind = TxtrKind::D2;
    u32 format = 0;              // index for txtr_format()
    u32 width = 0, height = 0;
    u32 layers = 0;              // HEAD "layers": array length, 6 for cube, depth for 3D, else 1
    u32 mips = 0;                // mip count
    u32 tile_mode = 0, swizzle = 0;
    u64 gpu_size = 0;            // bytes of the decompressed GPU buffer (META "decompressed size")
};

// Reads the header (HEAD chunk) and the buffer table (META) without touching pixel data. `e.type` must be "TXTR".
TxtrInfo txtr_read_info(const Pak& pak, const PakEntry& e);
// The tiled GPU buffer, gpu_size bytes: every buffer in the META read list is decompressed (LZSS or stored) into its place.
std::vector<u8> txtr_read_tiled(const Pak& pak, const PakEntry& e, const TxtrInfo& info);

}  // namespace mprs
