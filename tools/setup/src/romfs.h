// SPDX-License-Identifier: GPL-3.0-or-later
// RomFS image parser (the file system inside NCA romfs sections). Format: https://switchbrew.org/wiki/RomFS (documentation only).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "storage.h"

namespace mprs {

struct RomfsFile {
    std::string path;     // relative path, '/' separated, no leading slash, UTF-8, e.g. "Worlds/MP1/Foo.pak"
    u64 offset = 0;       // absolute offset of the data inside the image storage (file_data_offset already added)
    u64 size = 0;
};

class Romfs {
public:
    // `image` starts at the 0x50-byte RomFS header. Throws Error(Input) when a table lies outside the image, an entry offset is
    // out of range, the directory tree has a cycle or is deeper than 256, a file's data reaches past the image, a name is
    // empty, contains '/', '\\', ':' or NUL, or is "." / "..". Files come out in tree order (a directory's files first, then
    // its subdirectories, depth first), which is deterministic.
    static std::unique_ptr<Romfs> open(StoragePtr image);

    const std::vector<RomfsFile>& files() const { return files_; }
    const std::vector<std::string>& dirs() const { return dirs_; }   // every directory path (excluding the root), parent before child
    u64 total_file_bytes() const { return total_; }
    StoragePtr open_file(const RomfsFile& f) const;
    const StoragePtr& image() const { return image_; }
private:
    std::vector<RomfsFile> files_;
    std::vector<std::string> dirs_;
    u64 total_ = 0;
    StoragePtr image_;
};

}  // namespace mprs
