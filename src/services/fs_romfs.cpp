// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER (S3c, being written): see fs_romfs.h.
#include "fs_romfs.h"

namespace nxrt::services::fs {
struct RomfsImage::Impl {};
RomfsImage::RomfsImage() : p_(std::make_unique<Impl>()) {}
RomfsImage::~RomfsImage() = default;
bool RomfsImage::build(const std::string&, std::string* err) {
    if (err) *err = "not implemented";
    return false;
}
bool RomfsImage::built() const { return false; }
uint64_t RomfsImage::size() const { return 0; }
bool RomfsImage::read(uint64_t, void*, uint64_t) const { return false; }
RomfsImage::Stats RomfsImage::stats() const { return {}; }
bool RomfsImage::file_extent(const std::string&, uint64_t*, uint64_t*) const { return false; }
}  // namespace nxrt::services::fs
