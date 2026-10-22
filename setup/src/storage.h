// SPDX-License-Identifier: GPL-3.0-or-later
// Positional, thread-safe, read-only byte sources. Every container layer (file, NSP entry, XCI partition, NCA section after
// decryption, RomFS file) is an IStorage so the parsers do not care where bytes come from.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "common.h"

namespace mprs {

class IStorage {
public:
    virtual ~IStorage() = default;
    virtual u64 size() const = 0;
    // Reads exactly len bytes at off. Throws Error(Input) if off+len is past the end (a truncated dump), Error(Generic) on an
    // I/O error. Must be safe to call from several threads at once.
    virtual void read(u64 off, void* buf, size_t len) const = 0;
};
using StoragePtr = std::shared_ptr<const IStorage>;

// A file on disk (UTF-8 path). Positional ReadFile, no shared file pointer.
StoragePtr open_file_storage(const std::string& utf8_path);
// base[off, off+len). Throws if the range does not fit in base.
StoragePtr make_sub_storage(StoragePtr base, u64 off, u64 len);
StoragePtr make_memory_storage(std::vector<u8> bytes);

// Convenience: read into a vector.
std::vector<u8> read_all(const IStorage& s, u64 off, size_t len);

}  // namespace mprs
