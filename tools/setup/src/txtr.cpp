// SPDX-License-Identifier: GPL-3.0-or-later
// TXTR asset, see txtr.h. Header and buffer table from tools/survey/txtr_decode.py (load) and tools/survey/txtr_tally.py
// (FORMATS, KINDS, fmt_info). The read list maps onto the asset bytes as in refs/retrotool/lib/src/format/txtr.rs
// (TextureData::slice): an offset in the read list is relative to the start of the asset's RFRM, and the buffer offset is
// relative to the read entry. LZSS follows txtr_decode.py lzss() and refs/retrotool/lib/src/util/lzss.rs.
#include "txtr.h"

#include <cstring>
#include <stdexcept>

namespace mprs {
namespace {

// Same order as FORMATS in tools/survey/txtr_tally.py; block sizes and flags as fmt_info() there returns them.
const TxtrFormat kFormats[] = {
    {"R8Unorm", 1, 1, 1, false, false, false},  // 0
    {"R8Snorm", 1, 1, 1, false, false, false},  // 1
    {"R8Uint", 1, 1, 1, false, false, false},  // 2
    {"R8Sint", 1, 1, 1, false, false, false},  // 3
    {"R16Unorm", 1, 1, 2, false, false, false},  // 4
    {"R16Snorm", 1, 1, 2, false, false, false},  // 5
    {"R16Uint", 1, 1, 2, false, false, false},  // 6
    {"R16Sint", 1, 1, 2, false, false, false},  // 7
    {"R16Float", 1, 1, 2, false, false, false},  // 8
    {"R32Uint", 1, 1, 4, false, false, false},  // 9
    {"R32Sint", 1, 1, 4, false, false, false},  // 10
    {"Rgb8Unorm", 1, 1, 3, false, false, false},  // 11
    {"Rgba8Unorm", 1, 1, 4, false, false, false},  // 12
    {"Rgba8Srgb", 1, 1, 4, false, false, true},  // 13
    {"Rgba16Float", 1, 1, 8, false, false, false},  // 14
    {"Rgba32Float", 1, 1, 16, false, false, false},  // 15
    {"Depth16Unorm", 1, 1, 2, false, false, false},  // 16
    {"Depth16Unorm2", 1, 1, 2, false, false, false},  // 17
    {"Depth24S8Unorm", 1, 1, 4, false, false, false},  // 18
    {"Depth32Float", 1, 1, 4, false, false, false},  // 19
    {"Bc1Unorm", 4, 4, 8, false, true, false},  // 20
    {"Bc1Srgb", 4, 4, 8, false, true, true},  // 21
    {"Bc2Unorm", 4, 4, 16, false, true, false},  // 22
    {"Bc2Srgb", 4, 4, 16, false, true, true},  // 23
    {"Bc3Unorm", 4, 4, 16, false, true, false},  // 24
    {"Bc3Srgb", 4, 4, 16, false, true, true},  // 25
    {"Bc4Unorm", 4, 4, 8, false, true, false},  // 26
    {"Bc4Snorm", 4, 4, 8, false, true, false},  // 27
    {"Bc5Unorm", 4, 4, 16, false, true, false},  // 28
    {"Bc5Snorm", 4, 4, 16, false, true, false},  // 29
    {"Rg11B10Float", 1, 1, 4, false, false, false},  // 30
    {"R32Float", 1, 1, 4, false, false, false},  // 31
    {"Rg8Unorm", 1, 1, 2, false, false, false},  // 32
    {"Rg8Snorm", 1, 1, 2, false, false, false},  // 33
    {"Rg8Uint", 1, 1, 2, false, false, false},  // 34
    {"Rg8Sint", 1, 1, 2, false, false, false},  // 35
    {"Rg16Float", 1, 1, 4, false, false, false},  // 36
    {"Rg16Unorm", 1, 1, 4, false, false, false},  // 37
    {"Rg16Snorm", 1, 1, 4, false, false, false},  // 38
    {"Rg16Uint", 1, 1, 4, false, false, false},  // 39
    {"Rg16Sint", 1, 1, 4, false, false, false},  // 40
    {"Rgb10A2Unorm", 1, 1, 4, false, false, false},  // 41
    {"Rgb10A2Uint", 1, 1, 4, false, false, false},  // 42
    {"Rg32Uint", 1, 1, 8, false, false, false},  // 43
    {"Rg32Sint", 1, 1, 8, false, false, false},  // 44
    {"Rg32Float", 1, 1, 8, false, false, false},  // 45
    {"Rgba16Unorm", 1, 1, 8, false, false, false},  // 46
    {"Rgba16Snorm", 1, 1, 8, false, false, false},  // 47
    {"Rgba16Uint", 1, 1, 8, false, false, false},  // 48
    {"Rgba16Sint", 1, 1, 8, false, false, false},  // 49
    {"Rgba32Uint", 1, 1, 16, false, false, false},  // 50
    {"Rgba32Sint", 1, 1, 16, false, false, false},  // 51
    {"None", 1, 1, 0, false, false, false},  // 52
    {"Astc4x4", 4, 4, 16, true, false, false},  // 53
    {"Astc5x4", 5, 4, 16, true, false, false},  // 54
    {"Astc5x5", 5, 5, 16, true, false, false},  // 55
    {"Astc6x5", 6, 5, 16, true, false, false},  // 56
    {"Astc6x6", 6, 6, 16, true, false, false},  // 57
    {"Astc8x5", 8, 5, 16, true, false, false},  // 58
    {"Astc8x6", 8, 6, 16, true, false, false},  // 59
    {"Astc8x8", 8, 8, 16, true, false, false},  // 60
    {"Astc10x5", 10, 5, 16, true, false, false},  // 61
    {"Astc10x6", 10, 6, 16, true, false, false},  // 62
    {"Astc10x8", 10, 8, 16, true, false, false},  // 63
    {"Astc10x10", 10, 10, 16, true, false, false},  // 64
    {"Astc12x10", 12, 10, 16, true, false, false},  // 65
    {"Astc12x12", 12, 12, 16, true, false, false},  // 66
    {"Astc4x4Srgb", 4, 4, 16, true, false, true},  // 67
    {"Astc5x4Srgb", 5, 4, 16, true, false, true},  // 68
    {"Astc5x5Srgb", 5, 5, 16, true, false, true},  // 69
    {"Astc6x5Srgb", 6, 5, 16, true, false, true},  // 70
    {"Astc6x6Srgb", 6, 6, 16, true, false, true},  // 71
    {"Astc8x5Srgb", 8, 5, 16, true, false, true},  // 72
    {"Astc8x6Srgb", 8, 6, 16, true, false, true},  // 73
    {"Astc8x8Srgb", 8, 8, 16, true, false, true},  // 74
    {"Astc10x5Srgb", 10, 5, 16, true, false, true},  // 75
    {"Astc10x6Srgb", 10, 6, 16, true, false, true},  // 76
    {"Astc10x8Srgb", 10, 8, 16, true, false, true},  // 77
    {"Astc10x10Srgb", 10, 10, 16, true, false, true},  // 78
    {"Astc12x10Srgb", 12, 10, 16, true, false, true},  // 79
    {"Astc12x12Srgb", 12, 12, 16, true, false, true},  // 80
    {"BptcUfloat", 4, 4, 16, false, true, false},  // 81
    {"BptcSfloat", 4, 4, 16, false, true, false},  // 82
    {"BptcUnorm", 4, 4, 16, false, true, false},  // 83
    {"BptcUnormSrgb", 4, 4, 16, false, true, true},  // 84
};
constexpr size_t kFormatCount = sizeof kFormats / sizeof kFormats[0];
constexpr u32 kKindCount = 9;                    // KINDS in txtr_tally.py
constexpr u64 kMaxGpuBytes = u64(1) << 30;       // the largest real GPU buffer is about 34 MB; anything near 1 GiB is damage

// Chunk and form layout as in pak.cpp: form header 32 bytes (form id at +20, reader version +24), chunk header 24 bytes.
constexpr u64 kFormHeader = 32;
constexpr u64 kChunkHeader = 24;

u32 rd32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u64 rd64(const u8* p) { return u64(rd32(p)) | u64(rd32(p + 4)) << 32; }
bool fourcc_is(const u8* p, const char* id) { return std::memcmp(p, id, 4) == 0; }

// Decompresses one buffer straight into out (dst of out_len bytes). `in` is the buffer as stored: a u32 mode word, then data.
void decompress_into(const u8* in, size_t in_len, u8* out, size_t out_len) {
    if (in_len < 4) fail(Fail::Input, "A texture data buffer is only %zu bytes; it is too short to hold its own mode word.", in_len);
    const u32 mode = rd32(in);
    const u8* data = in + 4;
    const size_t n = in_len - 4;
    if (mode == 0) {
        if (n != out_len)
            fail(Fail::Input, "A stored (not compressed) texture buffer holds %zu bytes but %zu are expected.", n, out_len);
        if (n) std::memcpy(out, data, n);
        return;
    }
    if (mode < 1 || mode > 3) fail(Fail::Input, "A texture data buffer uses compression mode %u, which this tool does not know.", mode);

    // LZSS: a header byte gives the flags of the next 8 items, most significant bit first. A 0 flag is a literal group of g
    // bytes; a 1 flag is a back-reference of (b0 >> 4) + 4 - mode groups, starting length groups back (12-bit field).
    const size_t g = size_t(1) << (mode - 1);
    size_t i = 0, o = 0;
    u8 hdr = 0;
    unsigned grp = 0;
    while (i < n) {
        if (grp == 0) {
            hdr = data[i++];
            grp = 8;
        }
        if (!(hdr & 0x80)) {
            if (g > n - i) fail(Fail::Input, "A compressed texture buffer ends in the middle of a literal. The data is damaged.");
            if (g > out_len - o) fail(Fail::Input, "A compressed texture buffer decompresses to more than its declared size.");
            std::memcpy(out + o, data + i, g);
            i += g;
            o += g;
        } else {
            if (n - i < 2) fail(Fail::Input, "A compressed texture buffer ends in the middle of a back-reference. The data is damaged.");
            const size_t b0 = data[i], b1 = data[i + 1];
            i += 2;
            const size_t count = (b0 >> 4) + (4 - mode);
            const size_t length = (((b0 & 0xF) << 8) | b1) << (mode - 1);
            const size_t nb = count * g;
            if (length == 0) fail(Fail::Input, "A compressed texture buffer has a back-reference of length zero. The data is damaged.");
            if (length > o)
                fail(Fail::Input, "A compressed texture buffer refers to bytes before the start of its output. The data is damaged.");
            if (nb > out_len - o) fail(Fail::Input, "A compressed texture buffer decompresses to more than its declared size.");
            const size_t s = o - length;
            if (length >= nb) {
                std::memcpy(out + o, out + s, nb);  // source and destination do not overlap
            } else {
                for (size_t k = 0; k < nb; ++k) out[o + k] = out[s + k];  // overlapping: repeats the last `length` bytes
            }
            o += nb;
        }
        hdr = static_cast<u8>(hdr << 1);
        --grp;
    }
    if (o != out_len)
        fail(Fail::Input, "A compressed texture buffer decompresses to %zu bytes but %zu are expected. The data is damaged.", o,
             out_len);
}

}  // namespace

std::vector<u8> txtr_decompress(const u8* in, size_t in_len, size_t out_len) {
    std::vector<u8> out(out_len);
    decompress_into(in, in_len, out.data(), out.size());
    return out;
}

const TxtrFormat& txtr_format(u32 id) {
    if (id >= kFormatCount) fail(Fail::Input, "The texture uses format number %u, which this tool does not know.", id);
    return kFormats[id];
}

TxtrInfo txtr_read_info(const Pak& pak, const PakEntry& e) {
    if (std::strcmp(e.type, "TXTR") != 0)
        fail(Fail::Input, "The asset %s is a %s, not a texture (TXTR).", e.id.str().c_str(), e.type);
    if (e.size != e.decompressed_size)
        fail(Fail::Input, "The texture %s is compressed inside the archive; this tool does not read that.", e.id.str().c_str());
    if (e.size < 56)
        fail(Fail::Input, "The texture %s is too short to hold its header (%llu bytes).", e.id.str().c_str(),
             (unsigned long long)e.size);

    // Only the first 56 bytes (form header and the HEAD chunk header) are read here: the pixel data is not touched.
    u8 h[56];
    pak.file()->read(e.offset, h, sizeof h);
    if (!fourcc_is(h, "RFRM") || !fourcc_is(h + 20, "TXTR"))
        fail(Fail::Input, "The data of texture %s is not a TXTR form. The archive is damaged.", e.id.str().c_str());
    if (e.decompressed_size < kFormHeader || rd64(h + 4) != e.decompressed_size - kFormHeader)
        fail(Fail::Input, "The form size of texture %s does not match the archive directory.", e.id.str().c_str());
    if (!fourcc_is(h + kFormHeader, "HEAD"))
        fail(Fail::Input, "Texture %s has no HEAD chunk where expected. The archive is damaged.", e.id.str().c_str());
    const u64 head_size = rd64(h + kFormHeader + 4), head_skip = rd64(h + kFormHeader + 16);
    const u64 avail = e.size - 56;
    if (head_skip > avail || head_size > avail - head_skip)
        fail(Fail::Input, "The HEAD chunk of texture %s runs past the texture. The archive is damaged.", e.id.str().c_str());
    if (head_size < 32) fail(Fail::Input, "The HEAD chunk of texture %s is too short.", e.id.str().c_str());

    // HEAD body: kind, format, width, height, layers, tile mode, swizzle, mip count (8 x u32), then one u32 size per mip.
    const u64 body = e.offset + kFormHeader + kChunkHeader + head_skip;
    u8 f[32];
    pak.file()->read(body, f, sizeof f);
    const u32 nmip = rd32(f + 28);
    if (nmip > (head_size - 32) / 4)
        fail(Fail::Input, "The mip table of texture %s runs past its HEAD chunk. The archive is damaged.", e.id.str().c_str());
    TxtrInfo info;
    info.id = e.id;
    const u32 kind = rd32(f);
    if (kind >= kKindCount) fail(Fail::Input, "Texture %s has kind number %u, which this tool does not know.", e.id.str().c_str(), kind);
    info.kind = static_cast<TxtrKind>(kind);
    info.format = rd32(f + 4);
    txtr_format(info.format);  // throws for an unknown format
    info.width = rd32(f + 8);
    info.height = rd32(f + 12);
    info.layers = rd32(f + 16);
    info.tile_mode = rd32(f + 20);
    info.swizzle = rd32(f + 24);
    info.mips = nmip;

    // META (buffer table): decompressed size at +20.
    const std::vector<u8> meta = pak.read_meta(e);
    if (meta.size() < 28)
        fail(Fail::Input, "Texture %s has no usable buffer table (META). The archive is damaged.", e.id.str().c_str());
    info.gpu_size = rd32(meta.data() + 20);
    return info;
}

std::vector<u8> txtr_read_tiled(const Pak& pak, const PakEntry& e, const TxtrInfo& info) {
    if (std::strcmp(e.type, "TXTR") != 0)
        fail(Fail::Input, "The asset %s is a %s, not a texture (TXTR).", e.id.str().c_str(), e.type);
    const std::vector<u8> meta = pak.read_meta(e);
    if (meta.size() < 28)
        fail(Fail::Input, "Texture %s has no usable buffer table (META). The archive is damaged.", e.id.str().c_str());
    const u8* m = meta.data();
    const size_t mlen = meta.size();
    const u32 dec = rd32(m + 20);
    if (dec != info.gpu_size)
        fail(Fail::Input, "Texture %s: the buffer table says %u bytes, the header read said %llu.", e.id.str().c_str(), dec,
             (unsigned long long)info.gpu_size);
    if (u64(dec) > kMaxGpuBytes)
        fail(Fail::Input, "Texture %s would need %u bytes of GPU memory, which is implausibly large. The archive is damaged.",
             e.id.str().c_str(), dec);

    // Read list: count, then {u8 index, u32 offset, u32 size} each (9 bytes).
    size_t p = 24;
    const u32 ninfo = rd32(m + p);
    p += 4;
    if (ninfo > (mlen - p) / 9) fail(Fail::Input, "The read list of texture %s runs past its buffer table.", e.id.str().c_str());
    struct ReadEntry {
        u32 index, offset, size;
    };
    std::vector<ReadEntry> reads;
    reads.reserve(ninfo);
    for (u32 k = 0; k < ninfo; ++k, p += 9) reads.push_back({m[p], rd32(m + p + 1), rd32(m + p + 5)});

    // Buffer list: count, then {u32 index, u32 offset, u32 size, u32 dest offset, u32 dest size} each (20 bytes).
    if (p + 4 > mlen) fail(Fail::Input, "The buffer table of texture %s is cut short.", e.id.str().c_str());
    const u32 nbuf = rd32(m + p);
    p += 4;
    if (nbuf > (mlen - p) / 20) fail(Fail::Input, "The buffer list of texture %s runs past its buffer table.", e.id.str().c_str());

    const std::vector<u8> asset = pak.read_asset(e);  // the asset's own bytes; read offsets count from its first byte
    std::vector<u8> out(dec);
    for (u32 k = 0; k < nbuf; ++k, p += 20) {
        const u32 index = rd32(m + p), boff = rd32(m + p + 4), bsz = rd32(m + p + 8), doff = rd32(m + p + 12),
                  dsz = rd32(m + p + 16);
        const ReadEntry* rd = nullptr;
        for (const auto& r : reads) {
            if (r.index == index) {
                rd = &r;
                break;
            }
        }
        if (!rd) fail(Fail::Input, "Texture %s: no read entry for buffer %u. The archive is damaged.", e.id.str().c_str(), index);
        if (u64(rd->offset) + rd->size > asset.size())
            fail(Fail::Input, "Texture %s: a read entry reaches past the end of the texture. The archive is damaged.",
                 e.id.str().c_str());
        if (u64(boff) + bsz > rd->size)
            fail(Fail::Input, "Texture %s: a buffer reaches past its read entry. The archive is damaged.", e.id.str().c_str());
        if (u64(doff) + dsz > dec)
            fail(Fail::Input, "Texture %s: a buffer is placed past the end of the GPU buffer. The archive is damaged.",
                 e.id.str().c_str());
        decompress_into(asset.data() + rd->offset + boff, bsz, out.data() + doff, dsz);
    }
    return out;
}

}  // namespace mprs
