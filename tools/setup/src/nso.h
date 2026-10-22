// SPDX-License-Identifier: GPL-3.0-or-later
// NSO executable header: just enough to read the build id (module id) and sanity-check the file. The loader in the runtime does
// the real work; this tool copies the NSO files verbatim.
// Format: https://switchbrew.org/wiki/NSO (documentation only).
#pragma once
#include <string>

#include "storage.h"

namespace mprs {

struct NsoInfo {
    u32 flags = 0;
    u32 text_size = 0, ro_size = 0, data_size = 0;   // decompressed segment sizes
    std::array<u8, 32> module_id{};                  // the 0x20 bytes at header +0x40
    std::string build_id() const;                    // first 20 bytes of module_id as 40 UPPER-case hex digits
};

// Reads the 0x100-byte header. Throws Error(Input) if the file is shorter than 0x100 bytes, the magic is not "NSO0", or the
// compressed segment sizes do not add up to a file that fits in the storage (header 0x100 + text + ro + data compressed sizes
// must be <= storage size).
NsoInfo nso_read_info(const IStorage& file);

}  // namespace mprs
