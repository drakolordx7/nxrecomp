// SPDX-License-Identifier: GPL-3.0-or-later
// PFS0 (NSP, ExeFS, NCA program sections) and HFS0 (XCI partitions) directory parsers over an IStorage.
// Formats: https://switchbrew.org/wiki/NCA#PFS0 and https://switchbrew.org/wiki/XCI#HFS0 (documentation only, written fresh).
#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "storage.h"

namespace mprs {

struct PfsEntry {
    std::string name;   // as stored (UTF-8); never empty, never contains '/' or '\\', never "." or ".." (rejected at open)
    u64 offset = 0;     // byte offset of the file data inside the storage that was opened (absolute, header included)
    u64 size = 0;
    // HFS0 only: the partition header's hash fields. hashed_size is how many leading bytes of the file the SHA-256 covers.
    u32 hashed_size = 0;
    std::array<u8, 32> sha256{};
};

class Pfs {
public:
    enum class Kind { Pfs0, Hfs0 };
    // `storage` starts at the "PFS0" / "HFS0" magic. Throws Error(Input) on a bad magic, a header or string table that does not
    // fit, an entry that reaches past the end of the storage, a bad name, or more than 1,000,000 entries.
    static std::unique_ptr<Pfs> open(StoragePtr storage);

    Kind kind() const { return kind_; }
    u64 header_size() const { return header_size_; }   // bytes before the first file data (header + entry table + string table)
    const std::vector<PfsEntry>& entries() const { return entries_; }
    const PfsEntry* find(std::string_view name) const;       // exact match, nullptr if absent
    StoragePtr open_file(const PfsEntry& e) const;           // sub-storage over the file data
    const StoragePtr& storage() const { return storage_; }
private:
    Kind kind_ = Kind::Pfs0;
    u64 header_size_ = 0;
    std::vector<PfsEntry> entries_;
    StoragePtr storage_;
};

}  // namespace mprs
