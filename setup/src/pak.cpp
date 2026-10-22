// SPDX-License-Identifier: GPL-3.0-or-later
// PACK reader, see pak.h. Layout from tools/survey/inventory.py (read_adir) and refs/retrotool/lib/src/format/pack.rs.
#include "pak.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace mprs {
namespace {

constexpr u64 kFormHeader = 32;   // 'RFRM' + u64 size (+4) + 8 bytes + form id (+20) + reader version (+24) + writer version (+28)
constexpr u64 kChunkHeader = 24;  // chunk id (+0) + u64 size (+4) + u32 unknown (+12) + u64 skip (+16); body at +24+skip
constexpr u64 kAdirEntry = 52;    // fourcc, uuid, u32 version, u32 other version, u64 offset, u64 decompressed size, u64 size
constexpr u64 kMetaEntry = 20;    // uuid, u32 offset into the META chunk body

u32 rd32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u64 rd64(const u8* p) { return u64(rd32(p)) | u64(rd32(p + 4)) << 32; }
bool fourcc_is(const u8* p, const char* id) { return std::memcmp(p, id, 4) == 0; }

std::string fourcc_text(const u8* p) {
    std::string s;
    for (int i = 0; i < 4; ++i) s += (p[i] >= 0x20 && p[i] < 0x7f) ? char(p[i]) : '?';
    return s;
}

}  // namespace

// Pak declares this struct as its friend; it fills the private members while the archive is opened.
struct PakImpl {
    static void parse_adir(Pak& pak, const u8* body, u64 blen);
    static void parse_meta(Pak& pak, const u8* body, u64 blen);
};

std::string Uuid::str() const {
    char buf[37];
    std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return buf;
}

void PakImpl::parse_adir(Pak& pak, const u8* body, u64 blen) {
    if (blen < 4) fail(Fail::Input, "The asset directory (ADIR) of the archive is empty.");
    const u64 count = rd32(body);
    if (count > (blen - 4) / kAdirEntry)
        fail(Fail::Input, "The asset directory (ADIR) lists %llu assets but only has room for %llu. The archive is damaged.",
             (unsigned long long)count, (unsigned long long)((blen - 4) / kAdirEntry));
    pak.assets_.reserve(static_cast<size_t>(count));
    for (u64 i = 0; i < count; ++i) {
        const u8* e = body + 4 + i * kAdirEntry;
        PakEntry pe;
        std::memcpy(pe.type, e, 4);
        pe.type[4] = 0;
        std::memcpy(pe.id.b, e + 4, 16);
        pe.version = rd32(e + 20);
        pe.other_version = rd32(e + 24);
        pe.offset = rd64(e + 28);
        pe.decompressed_size = rd64(e + 36);
        pe.size = rd64(e + 44);
        pak.assets_.push_back(pe);
    }
}

void PakImpl::parse_meta(Pak& pak, const u8* body, u64 blen) {
    pak.meta_chunk_.assign(body, body + blen);
    if (blen < 4) fail(Fail::Input, "The metadata table (META) of the archive is empty.");
    const u64 count = rd32(body);
    if (count > (blen - 4) / kMetaEntry)
        fail(Fail::Input, "The metadata table (META) lists %llu records but only has room for %llu. The archive is damaged.",
             (unsigned long long)count, (unsigned long long)((blen - 4) / kMetaEntry));
    pak.meta_index_.reserve(static_cast<size_t>(count));
    for (u64 i = 0; i < count; ++i) {
        const u8* e = body + 4 + i * kMetaEntry;
        Uuid id;
        std::memcpy(id.b, e, 16);
        const u64 off = rd32(e + 16);
        if (off + 4 > blen) fail(Fail::Input, "A metadata record points past the end of the META chunk. The archive is damaged.");
        const u64 msz = rd32(body + off);
        if (msz > blen - off - 4) fail(Fail::Input, "A metadata record is longer than the META chunk. The archive is damaged.");
        pak.meta_index_.emplace_back(id, static_cast<u32>(off));
    }
}

std::unique_ptr<Pak> Pak::open(StoragePtr file) {
    const u64 fsize = file->size();
    if (fsize < 2 * kFormHeader)
        fail(Fail::Input, "The file is too small to be a Retro PACK archive (%llu bytes).", (unsigned long long)fsize);

    u8 pack_hdr[kFormHeader];
    file->read(0, pack_hdr, sizeof pack_hdr);
    if (!fourcc_is(pack_hdr, "RFRM") || !fourcc_is(pack_hdr + 20, "PACK"))
        fail(Fail::Input, "This is not a Retro PACK archive (it starts with \"%s\", not a PACK form).",
             fourcc_text(pack_hdr + 20).c_str());
    if (rd32(pack_hdr + 24) != 1)
        fail(Fail::Input, "The PACK archive uses a version this tool does not read (%u, expected 1).", rd32(pack_hdr + 24));

    u8 toc_hdr[kFormHeader];
    file->read(kFormHeader, toc_hdr, sizeof toc_hdr);
    if (!fourcc_is(toc_hdr, "RFRM") || !fourcc_is(toc_hdr + 20, "TOCC"))
        fail(Fail::Input, "The PACK archive has no table of contents (TOCC) where expected.");
    if (rd32(toc_hdr + 24) != 3)
        fail(Fail::Input, "The table of contents uses a version this tool does not read (%u, expected 3).", rd32(toc_hdr + 24));
    const u64 toc_size = rd64(toc_hdr + 4);
    if (toc_size > fsize - 2 * kFormHeader)
        fail(Fail::Input, "The table of contents runs past the end of the file (%llu bytes declared). The dump is truncated.",
             (unsigned long long)toc_size);

    std::vector<u8> toc(static_cast<size_t>(toc_size));
    if (!toc.empty()) file->read(2 * kFormHeader, toc.data(), toc.size());

    auto pak = std::make_unique<Pak>();
    pak->file_ = std::move(file);
    bool have_adir = false, have_meta = false;
    u64 off = 0;
    while (off + kChunkHeader <= toc_size) {
        const u8* h = toc.data() + off;
        const u64 size = rd64(h + 4), skip = rd64(h + 16);
        const u64 avail = toc_size - off - kChunkHeader;
        if (skip > avail || size > avail - skip)
            fail(Fail::Input, "A chunk (%s) in the table of contents runs past its end. The archive is damaged.",
                 fourcc_text(h).c_str());
        const u8* body = h + kChunkHeader + skip;
        if (fourcc_is(h, "ADIR")) {
            if (have_adir) fail(Fail::Input, "The table of contents has two asset directories. The archive is damaged.");
            have_adir = true;
            PakImpl::parse_adir(*pak, body, size);
        } else if (fourcc_is(h, "META")) {
            if (have_meta) fail(Fail::Input, "The table of contents has two metadata tables. The archive is damaged.");
            have_meta = true;
            PakImpl::parse_meta(*pak, body, size);
        }
        // Other chunks (the STRG name table and anything else) are not needed by this tool and are skipped.
        off += kChunkHeader + skip + size;
    }
    if (!have_adir) fail(Fail::Input, "The archive has no asset directory (ADIR). The archive is damaged.");
    return pak;
}

std::vector<u8> Pak::read_asset(const PakEntry& e) const {
    if (e.size != e.decompressed_size)
        fail(Fail::Input, "The %s asset %s is compressed inside the archive; this tool does not read that.", e.type,
             e.id.str().c_str());
    const u64 fsize = file_->size();
    if (e.offset > fsize || e.size > fsize - e.offset)
        fail(Fail::Input, "The %s asset %s points past the end of the archive (offset %llu, size %llu, file %llu bytes). "
                          "The dump is probably truncated.",
             e.type, e.id.str().c_str(), (unsigned long long)e.offset, (unsigned long long)e.size, (unsigned long long)fsize);
    if (e.size < kFormHeader || e.size > std::numeric_limits<size_t>::max())
        fail(Fail::Input, "The %s asset %s has an impossible size (%llu bytes).", e.type, e.id.str().c_str(),
             (unsigned long long)e.size);

    std::vector<u8> out(static_cast<size_t>(e.size));
    file_->read(e.offset, out.data(), out.size());

    // The asset must be the form the directory says it is (same checks as pack.rs read_asset).
    if (!fourcc_is(out.data(), "RFRM") || !fourcc_is(out.data() + 20, e.type))
        fail(Fail::Input, "The data of asset %s is not a %s form. The archive is damaged.", e.id.str().c_str(), e.type);
    if (e.decompressed_size < kFormHeader || rd64(out.data() + 4) != e.decompressed_size - kFormHeader)
        fail(Fail::Input, "The form size inside asset %s does not match the archive directory.", e.id.str().c_str());
    if (rd32(out.data() + 24) != e.version || rd32(out.data() + 28) != e.other_version)
        fail(Fail::Input, "The form versions inside asset %s do not match the archive directory.", e.id.str().c_str());
    return out;
}

std::vector<u8> Pak::read_meta(const PakEntry& e) const {
    bool found = false;
    u32 off = 0;
    for (const auto& [id, o] : meta_index_) {
        if (id == e.id) {  // the last record wins, as in pack.rs read_full
            found = true;
            off = o;
        }
    }
    if (!found) return {};
    const u8* p = meta_chunk_.data() + off;  // bounds were checked in parse_meta
    const u32 n = rd32(p);
    return std::vector<u8>(p + 4, p + 4 + n);
}

}  // namespace mprs
