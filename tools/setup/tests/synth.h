// SPDX-License-Identifier: GPL-3.0-or-later
// Builders for synthetic PFS0 / HFS0 / RomFS images, used by the container tests. They write the formats as documented in
// pfs.h and romfs.h and do not validate their input (tests use them to build broken containers on purpose).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mprs_test {

using SynthFiles = std::vector<std::pair<std::string, std::vector<uint8_t>>>;

// Files are stored in the given order. extra_string_pad adds that many zero bytes after the names (inside the string table).
std::vector<uint8_t> synth_pfs0(const SynthFiles& files, uint32_t extra_string_pad = 0);
// Same, with HFS0 entries: hashed_size = min(size, 0x200) and sha256 over those leading bytes.
std::vector<uint8_t> synth_hfs0(const SynthFiles& files);
// Paths use '/'. Folders are created from the path components, in order of first appearance; a folder's files are listed in
// insertion order, then its subfolders. Hash tables are all 0xFFFFFFFF. Metadata tables are padded to 4; file data is
// 0x10-aligned and starts after the tables.
std::vector<uint8_t> synth_romfs(const SynthFiles& files_with_slash_paths);

// SHA-256 used by synth_hfs0 (small standalone implementation, so tests can check the stored hashes).
std::array<uint8_t, 32> synth_sha256(const uint8_t* data, size_t len);

}  // namespace mprs_test
