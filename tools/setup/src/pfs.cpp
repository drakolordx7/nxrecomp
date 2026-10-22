// SPDX-License-Identifier: GPL-3.0-or-later
#include "pfs.h"

#include <cstring>

namespace mprs {
namespace {

constexpr u32 kMaxFiles = 1000000;
constexpr u64 kPrefixSize = 0x10;      // "PFS0"/"HFS0", file_count, string_table_size, reserved
constexpr u64 kPfsEntrySize = 0x18;
constexpr u64 kHfsEntrySize = 0x40;

u32 rd32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u64 rd64(const u8* p) { return u64(rd32(p)) | u64(rd32(p + 4)) << 32; }

// off + len lies inside total. Written so no sum can overflow.
bool fits(u64 off, u64 len, u64 total) { return off <= total && len <= total - off; }

bool valid_name(std::string_view n) {
    if (n.empty() || n == "." || n == "..") return false;
    return n.find_first_of("/\\:") == std::string_view::npos;
}

}  // namespace

std::unique_ptr<Pfs> Pfs::open(StoragePtr storage) {
    const u64 total = storage->size();
    if (total < kPrefixSize)
        fail(Fail::Input, "The package is damaged: it is only %llu bytes, too small to hold its header.",
             (unsigned long long)total);
    u8 pre[kPrefixSize];
    storage->read(0, pre, sizeof pre);

    Kind kind;
    u64 entry_size;
    if (std::memcmp(pre, "PFS0", 4) == 0) {
        kind = Kind::Pfs0;
        entry_size = kPfsEntrySize;
    } else if (std::memcmp(pre, "HFS0", 4) == 0) {
        kind = Kind::Hfs0;
        entry_size = kHfsEntrySize;
    } else {
        fail(Fail::Input, "This is not a package file: its first four bytes are not \"PFS0\" or \"HFS0\". The dump is probably damaged.");
    }

    const u32 count = rd32(pre + 4);
    const u32 strtab_size = rd32(pre + 8);
    if (count > kMaxFiles)
        fail(Fail::Input, "The package claims %u files, which is far more than a real package holds. The package is damaged.", count);

    const u64 table_end = kPrefixSize + entry_size * count;
    if (table_end > total)
        fail(Fail::Input, "The package is damaged: its file table runs past the end (the table ends at byte %llu, the package is %llu bytes).",
             (unsigned long long)table_end, (unsigned long long)total);
    const u64 header_size = table_end + strtab_size;
    if (header_size > total)
        fail(Fail::Input, "The package is damaged: its name table runs past the end (the header ends at byte %llu, the package is %llu bytes).",
             (unsigned long long)header_size, (unsigned long long)total);

    const std::vector<u8> hdr = read_all(*storage, 0, size_t(header_size));
    const u8* strtab = hdr.data() + table_end;

    std::unique_ptr<Pfs> out(new Pfs());
    out->kind_ = kind;
    out->header_size_ = header_size;
    out->storage_ = storage;
    out->entries_.reserve(count);

    for (u32 i = 0; i < count; ++i) {
        const u8* e = hdr.data() + kPrefixSize + entry_size * i;
        PfsEntry pe;
        const u64 rel = rd64(e);
        const u64 sz = rd64(e + 8);
        const u32 str_off = rd32(e + 16);
        if (kind == Kind::Hfs0) {
            pe.hashed_size = rd32(e + 20);
            std::memcpy(pe.sha256.data(), e + 0x20, pe.sha256.size());
        }

        if (str_off >= strtab_size)
            fail(Fail::Input, "The package is damaged: file number %u has a name position outside the name table.", i);
        const u8* name_at = strtab + str_off;
        const void* zero = std::memchr(name_at, 0, strtab_size - str_off);
        if (!zero)
            fail(Fail::Input, "The package is damaged: file number %u has a name that is not ended inside the name table.", i);
        pe.name.assign(reinterpret_cast<const char*>(name_at), size_t(static_cast<const u8*>(zero) - name_at));

        if (pe.name.empty())
            fail(Fail::Input, "The package is damaged: file number %u has no name.", i);
        if (!valid_name(pe.name))
            fail(Fail::Input, "The package is damaged: file number %u has an invalid name (\"%s\").", i, pe.name.c_str());

        // Data starts right after the header; the file must fit inside the package.
        if (!fits(header_size, rel, total) || !fits(header_size + rel, sz, total))
            fail(Fail::Input, "The package is damaged: file \"%s\" runs past the end of the package (its data is at byte %llu, %llu bytes long, the package is %llu bytes).",
                 pe.name.c_str(), (unsigned long long)(header_size + rel), (unsigned long long)sz, (unsigned long long)total);
        pe.offset = header_size + rel;
        pe.size = sz;

        if (kind == Kind::Hfs0 && pe.hashed_size > sz)
            fail(Fail::Input, "The package is damaged: file \"%s\" says more of it is hashed (%u bytes) than the file contains (%llu bytes).",
                 pe.name.c_str(), pe.hashed_size, (unsigned long long)sz);
        out->entries_.push_back(std::move(pe));
    }
    return out;
}

const PfsEntry* Pfs::find(std::string_view name) const {
    for (const PfsEntry& e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

StoragePtr Pfs::open_file(const PfsEntry& e) const { return make_sub_storage(storage_, e.offset, e.size); }

}  // namespace mprs
