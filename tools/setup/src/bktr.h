// SPDX-License-Identifier: GPL-3.0-or-later
// Update (patch) RomFS: the bucket trees of a patch NCA section ("BKTR"), the IndirectStorage that lays the update's data over the
// base game's RomFS section, and the AES-CTR-EX storage that decrypts the update's data with a counter generation per region.
// Format: switchbrew.org/wiki/NCA and nstool / libpietendo (MIT, jakcron) read as documentation; written fresh.
#pragma once
#include <array>
#include <vector>

#include "storage.h"

namespace nxs {

constexpr u64 kBucketNodeSize = 0x4000;
// Largest table this reader takes: one index node plus as many entry sets as it can list.
constexpr u64 kMaxBucketTableSize = (1 + (kBucketNodeSize - 0x10) / 8) * kBucketNodeSize;

struct IndirectEntry {
    u64 virt = 0;          // offset in the patched section
    u64 phys = 0;          // offset in the source storage
    u32 storage = 0;       // 0 = base RomFS section, 1 = the update's data
};

struct AesCtrExEntry {
    u64 offset = 0;        // offset in the update's section
    u8 encryption = 0;     // 0 encrypted, 1 plain
    u32 generation = 0;    // goes into bytes 4..7 of the counter
};

// Parses a bucket tree table (node size 0x4000, one L1 node then the entry sets). `entry_count` is the count from the section
// header's BKTR header. Throws Error(Input) when the table is damaged or uses a layout this reader does not know (L2 nodes).
// end_offset is the end of the last entry (the L1 node's end offset).
std::vector<IndirectEntry> parse_indirect_table(const std::vector<u8>& table, u32 entry_count, u64& end_offset);
std::vector<AesCtrExEntry> parse_aes_ctr_ex_table(const std::vector<u8>& table, u32 entry_count, u64& end_offset);

// AES-128-CTR over `raw`: counter = ctr_hi (8 bytes) + big-endian (nca_offset + offset) / 16.
StoragePtr make_aes_ctr_storage(StoragePtr raw, const Key128& key, const std::array<u8, 8>& ctr_hi, u64 nca_offset);
// AES-CTR-EX over `raw` (the update's section): each entry's region uses ctr_hi with bytes 4..7 set to the entry's generation.
// Entries must be sorted; the last one ends at end_offset. Reads outside [entries[0].offset, end_offset) throw.
StoragePtr make_aes_ctr_ex_storage(StoragePtr raw, const Key128& key, const std::array<u8, 8>& ctr_hi, u64 nca_offset,
                                   std::vector<AesCtrExEntry> entries, u64 end_offset);
// The patched section: [0, size) maps through the entries to `base` (storage 0) or `patch` (storage 1). Entries sorted by virt,
// the first at 0.
StoragePtr make_indirect_storage(std::vector<IndirectEntry> entries, u64 size, StoragePtr base, StoragePtr patch);

}  // namespace nxs
