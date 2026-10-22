// SPDX-License-Identifier: GPL-3.0-or-later
#include "nso.h"

#include <cstring>

#include "common.h"

namespace mprs {
namespace {

u32 le32(const u8* p) {
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 | static_cast<u32>(p[3]) << 24;
}

constexpr u64 kHeaderSize = 0x100;

}  // namespace

std::string NsoInfo::build_id() const { return hex_encode(module_id.data(), 20, true); }

NsoInfo nso_read_info(const IStorage& file) {
    if (file.size() < kHeaderSize)
        fail(Fail::Input, "The NSO file is too short (%llu bytes, a header alone is 256 bytes).",
             static_cast<unsigned long long>(file.size()));

    u8 h[kHeaderSize];
    file.read(0, h, sizeof h);
    if (std::memcmp(h, "NSO0", 4) != 0) fail(Fail::Input, "This is not an NSO executable (the signature is wrong).");

    NsoInfo info;
    info.flags = le32(h + 0x0C);
    // Segment headers: text at +0x10, ro at +0x20, data at +0x30; each {file_offset, memory_offset, decompressed_size}.
    info.text_size = le32(h + 0x10 + 8);
    info.ro_size = le32(h + 0x20 + 8);
    info.data_size = le32(h + 0x30 + 8);
    std::memcpy(info.module_id.data(), h + 0x40, info.module_id.size());

    u64 compressed_end = kHeaderSize + static_cast<u64>(le32(h + 0x60)) + le32(h + 0x64) + le32(h + 0x68);
    if (compressed_end > file.size())
        fail(Fail::Input, "The NSO file is cut short: its segments need %llu bytes but the file has %llu.",
             static_cast<unsigned long long>(compressed_end), static_cast<unsigned long long>(file.size()));
    return info;
}

}  // namespace mprs
