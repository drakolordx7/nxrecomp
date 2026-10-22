// SPDX-License-Identifier: GPL-3.0-or-later
// NCZ reader: the zstd-compressed NCA of NSZ / XCZ dumps, format as written by the nsz project (see ncz.h).
// Built with zstd (MPRS_HAVE_ZSTD); without it open_ncz refuses the file.
#include "ncz.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "crypto.h"
#include "storage.h"

#ifdef MPRS_HAVE_ZSTD
#include <zstd.h>
#endif

namespace mprs {

#ifndef MPRS_HAVE_ZSTD

StoragePtr open_ncz(StoragePtr) {
    fail(Fail::Input, "Compressed dumps (NSZ / XCZ) are not supported by this build. Decompress it to an NSP first.");
}

#else

namespace {

constexpr u64 kHeaderSize = 0x4000;    // raw NCA header region, stored verbatim at the start of the NCZ
constexpr u64 kTableOffset = 0x4010;   // "NCZSECTN", u64 section count, then the 0x40-byte section entries
constexpr u64 kEntrySize = 0x40;       // u64 offset, u64 size, u64 crypto type, u64 padding, key[16], counter[16]
constexpr u64 kMaxSections = 64;
constexpr u64 kBlockHeaderSize = 0x18; // "NCZBLOCK", version, type, unused, block size exponent, u32 blocks, u64 size
constexpr size_t kBlockCacheSlots = 4;

u64 le64(const u8* p) {
    u64 v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
u32 le32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
void store_be64(u8* p, u64 v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<u8>(v >> (56 - 8 * i));
}

// One run of the original NCA: bytes [begin, end). A CTR run was encrypted with its section key and counter in the
// original NCA; the NCZ holds it decrypted, so reads encrypt it again.
struct Segment {
    u64 begin = 0, end = 0;
    bool ctr = false;
    u8 key[16]{};
    u8 nonce[16]{};   // the section's 16-byte counter; only its first 8 bytes are kept, as nsz does
};

// Encrypts (or decrypts, same operation) buf, which holds NCA bytes [abs, abs + len), with the CTR stream of s.
// Counter block = nonce[0..8] || big-endian64(abs / 16); the first abs % 16 key stream bytes are skipped.
void ctr_apply(const Segment& s, u64 abs, u8* buf, size_t len) {
    u8 ctr[16];
    std::memcpy(ctr, s.nonce, 8);
    store_be64(ctr + 8, abs >> 4);
    const size_t skip = static_cast<size_t>(abs & 15);
    if (skip == 0) {
        aes_ctr_crypt(s.key, ctr, buf, len);
        return;
    }
    std::vector<u8> tmp(skip + len, 0);
    std::memcpy(tmp.data() + skip, buf, len);
    aes_ctr_crypt(s.key, ctr, tmp.data(), tmp.size());
    std::memcpy(buf, tmp.data() + skip, len);
}

struct ZstdDStreamDeleter {
    void operator()(ZSTD_DStream* d) const { ZSTD_freeDStream(d); }
};

[[noreturn]] void damaged(u64 at) {
    fail(Fail::Input, "The compressed dump is damaged (its compressed data is corrupt near body byte %llu). "
                      "Re-download or re-dump the NSZ / XCZ file.", static_cast<unsigned long long>(at));
}

class NczStorage final : public IStorage {
public:
    // segs must cover [kHeaderSize, size) in order without gaps. body_start is the file offset of the compressed body.
    NczStorage(StoragePtr ncz, std::vector<Segment> segs, u64 size, u64 body_start, bool blocks)
        : ncz_(std::move(ncz)), segs_(std::move(segs)), size_(size), body_start_(body_start), body_len_(size - kHeaderSize),
          blocks_(blocks) {
        if (blocks_) {
            parse_block_table();
        } else {
            ds_.reset(ZSTD_createDStream());
            if (!ds_) fail(Fail::Generic, "Cannot start the zstd decoder (out of memory).");
            ZSTD_DCtx_reset(ds_.get(), ZSTD_reset_session_only);
            in_buf_.resize(ZSTD_DStreamInSize());
            scratch_.resize(ZSTD_DStreamOutSize());
            file_pos_ = body_start_;
        }
    }

    u64 size() const override { return size_; }

    void read(u64 off, void* buf, size_t len) const override {
        if (off > size_ || len > size_ - off)
            fail(Fail::Input, "Read past the end of a compressed NCA (offset %llu, %llu bytes, size %llu). "
                              "The dump is probably truncated.",
                 static_cast<unsigned long long>(off), static_cast<unsigned long long>(len),
                 static_cast<unsigned long long>(size_));
        if (len == 0) return;
        u8* out = static_cast<u8*>(buf);
        if (off < kHeaderSize) {
            const size_t n = static_cast<size_t>(std::min<u64>(len, kHeaderSize - off));
            ncz_->read(off, out, n);   // the header region is stored as is
            out += n;
            off += n;
            len -= n;
            if (len == 0) return;
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (blocks_) {
            block_read(off - kHeaderSize, out, len);
        } else {
            solid_read(off - kHeaderSize, out, len);
        }
        const u64 end = off + len;
        for (const Segment& s : segs_) {
            if (!s.ctr || s.end <= off || s.begin >= end) continue;
            const u64 a = std::max(s.begin, off);
            const u64 b = std::min(s.end, end);
            ctr_apply(s, a, out + (a - off), static_cast<size_t>(b - a));
        }
    }

private:
    struct CachedBlock {
        u64 index = 0;
        u64 used = 0;
        std::shared_ptr<const std::vector<u8>> data;
    };

    void parse_block_table() {
        u8 h[kBlockHeaderSize];
        if (ncz_->size() < body_start_ + kBlockHeaderSize)
            fail(Fail::Input, "The compressed dump is truncated inside its block header. The dump is damaged or incomplete.");
        ncz_->read(body_start_, h, sizeof h);
        const u8 exponent = h[11];
        const u32 nblocks = le32(h + 12);
        const u64 dsize = le64(h + 16);
        if (exponent < 14 || exponent > 32)
            fail(Fail::Input, "The compressed dump is damaged: its block size 2^%u is out of range (2^14 to 2^32).",
                 static_cast<unsigned>(exponent));
        block_size_ = u64(1) << exponent;
        const u64 expected = body_len_ / block_size_ + (body_len_ % block_size_ != 0 ? 1 : 0);
        if (dsize != body_len_ || nblocks != expected)
            fail(Fail::Input, "The compressed dump's block table does not match the NCA size (%u blocks for %llu bytes, "
                              "expected %llu blocks for %llu bytes). The dump is damaged.",
                 static_cast<unsigned>(nblocks), static_cast<unsigned long long>(dsize),
                 static_cast<unsigned long long>(expected), static_cast<unsigned long long>(body_len_));
        const u64 list_at = body_start_ + kBlockHeaderSize;
        const u64 data_at = list_at + 4ull * nblocks;
        if (data_at > ncz_->size())
            fail(Fail::Input, "The compressed dump is truncated inside its block table. The dump is damaged or incomplete.");
        std::vector<u8> list(static_cast<size_t>(4ull * nblocks));
        if (!list.empty()) ncz_->read(list_at, list.data(), list.size());
        block_csz_.resize(nblocks);
        block_off_.resize(nblocks);
        u64 at = data_at;
        for (u32 i = 0; i < nblocks; ++i) {
            block_csz_[i] = le32(list.data() + 4ull * i);
            block_off_[i] = at;
            at += block_csz_[i];
            if (at > ncz_->size())
                fail(Fail::Input, "The compressed dump is truncated: block %u ends past the end of the file. "
                                  "The dump is damaged or incomplete.", static_cast<unsigned>(i));
        }
    }

    // Plain body bytes [bpos, bpos + len) from block-compressed data.
    void block_read(u64 bpos, u8* out, size_t len) const {
        while (len > 0) {
            const u64 idx = bpos / block_size_;
            const u64 within = bpos % block_size_;
            const auto blk = load_block(idx);
            const size_t n = static_cast<size_t>(std::min<u64>(len, blk->size() - within));
            std::memcpy(out, blk->data() + within, n);
            out += n;
            bpos += n;
            len -= n;
        }
    }

    // Returns block idx decoded, from the cache when it is there. A block whose stored size is below its decoded size is
    // a zstd frame; otherwise the block is stored raw (nsz writes it that way when compression does not shrink it).
    std::shared_ptr<const std::vector<u8>> load_block(u64 idx) const {
        const u64 used = ++cache_clock_;
        for (CachedBlock& c : cache_) {
            if (c.data && c.index == idx) {
                c.used = used;
                return c.data;
            }
        }
        const u64 dsize = std::min<u64>(block_size_, body_len_ - idx * block_size_);
        const u64 csize = block_csz_[idx];
        auto data = std::make_shared<std::vector<u8>>(static_cast<size_t>(dsize));
        if (csize < dsize) {
            std::vector<u8> comp(static_cast<size_t>(csize));
            if (csize) ncz_->read(block_off_[idx], comp.data(), comp.size());
            const size_t got = ZSTD_decompress(data->data(), data->size(), comp.data(), comp.size());
            if (ZSTD_isError(got) || got != dsize) damaged(idx * block_size_);
        } else {
            ncz_->read(block_off_[idx], data->data(), data->size());
        }
        CachedBlock* slot = nullptr;
        for (CachedBlock& c : cache_) {
            if (!c.data) {
                slot = &c;
                break;
            }
        }
        if (!slot) {
            slot = &cache_[0];
            for (CachedBlock& c : cache_) {
                if (c.used < slot->used) slot = &c;
            }
        }
        slot->index = idx;
        slot->used = used;
        slot->data = data;
        return data;
    }

    // Plain body bytes [bpos, bpos + len) from the solid zstd stream. The stream only moves forward; a request behind
    // its position restarts it from the beginning, and a request ahead decodes and discards the gap.
    void solid_read(u64 bpos, u8* out, size_t len) const {
        if (broken_ || bpos < pos_) {
            ZSTD_DCtx_reset(ds_.get(), ZSTD_reset_session_only);
            in_pos_ = 0;
            in_size_ = 0;
            file_pos_ = body_start_;
            pos_ = 0;
            broken_ = false;
        }
        while (pos_ < bpos) {
            const size_t n = static_cast<size_t>(std::min<u64>(bpos - pos_, scratch_.size()));
            const size_t got = solid_produce(scratch_.data(), n);
            pos_ += got;
            if (got < n) truncated(pos_);
        }
        const size_t got = solid_produce(out, len);
        pos_ += got;
        if (got < len) truncated(pos_);
    }

    // Decodes up to want bytes into dst. Returns how many came out; less only at the end of the compressed data.
    size_t solid_produce(u8* dst, size_t want) const {
        ZSTD_outBuffer ob{dst, want, 0};
        while (ob.pos < ob.size) {
            if (in_pos_ == in_size_) {
                if (file_pos_ >= ncz_->size()) break;
                const size_t n = static_cast<size_t>(std::min<u64>(in_buf_.size(), ncz_->size() - file_pos_));
                ncz_->read(file_pos_, in_buf_.data(), n);
                file_pos_ += n;
                in_pos_ = 0;
                in_size_ = n;
            }
            ZSTD_inBuffer ib{in_buf_.data(), in_size_, in_pos_};
            const size_t r = ZSTD_decompressStream(ds_.get(), &ob, &ib);
            in_pos_ = ib.pos;
            if (ZSTD_isError(r)) {
                broken_ = true;
                damaged(pos_ + ob.pos);
            }
        }
        return ob.pos;
    }

    [[noreturn]] void truncated(u64 at) const {
        fail(Fail::Input, "The compressed dump is damaged or truncated (its compressed data ends at body byte %llu of %llu). "
                          "Re-download or re-dump the NSZ / XCZ file.",
             static_cast<unsigned long long>(at), static_cast<unsigned long long>(body_len_));
    }

    StoragePtr ncz_;
    std::vector<Segment> segs_;
    u64 size_;
    u64 body_start_;   // file offset of the compressed body (right after the section table)
    u64 body_len_;     // size_ - kHeaderSize: decoded body bytes
    bool blocks_;

    // Block mode (set once in the constructor).
    u64 block_size_ = 0;
    std::vector<u64> block_off_;   // file offset of each stored block
    std::vector<u32> block_csz_;   // stored size of each block

    // Everything below changes while reading; guarded by mu_.
    mutable std::mutex mu_;
    mutable CachedBlock cache_[kBlockCacheSlots];
    mutable u64 cache_clock_ = 0;
    mutable std::unique_ptr<ZSTD_DStream, ZstdDStreamDeleter> ds_;   // solid mode
    mutable u64 pos_ = 0;          // solid mode: body bytes the stream has produced so far
    mutable u64 file_pos_ = 0;     // solid mode: next compressed byte to feed the decoder
    mutable std::vector<u8> in_buf_;
    mutable size_t in_pos_ = 0, in_size_ = 0;
    mutable std::vector<u8> scratch_;
    mutable bool broken_ = false;  // a decode error left the stream unusable until the next reset
};

}  // namespace

StoragePtr open_ncz(StoragePtr ncz) {
    const u64 file = ncz->size();
    if (file < kTableOffset)
        fail(Fail::Input, "This file is too short to be a compressed NCA (%llu bytes). The dump is damaged or truncated.",
             static_cast<unsigned long long>(file));
    u8 head[16];
    ncz->read(kHeaderSize, head, sizeof head);
    if (std::memcmp(head, "NCZSECTN", 8) != 0)
        fail(Fail::Input, "This is not a compressed NCA: the NCZSECTN marker is missing. "
                          "The file is damaged or is not an NSZ / XCZ dump.");
    const u64 count = le64(head + 8);
    if (count == 0 || count > kMaxSections)
        fail(Fail::Input, "The compressed dump lists %llu sections; an NCA has 1 to 64. The dump is damaged.",
             static_cast<unsigned long long>(count));
    const u64 table_end = kTableOffset + count * kEntrySize;
    if (table_end > file)
        fail(Fail::Input, "The compressed dump is truncated: its section table needs %llu bytes but the file has %llu.",
             static_cast<unsigned long long>(table_end), static_cast<unsigned long long>(file));
    std::vector<u8> table(static_cast<size_t>(count * kEntrySize));
    ncz->read(kTableOffset, table.data(), table.size());

    // The NCA is the header region, then (if the first section starts later) a plain gap, then the sections back to back.
    std::vector<Segment> segs;
    u64 pos = kHeaderSize;
    for (u64 i = 0; i < count; ++i) {
        const u8* e = table.data() + i * kEntrySize;
        const u64 offset = le64(e);
        const u64 size = le64(e + 8);
        const u64 type = le64(e + 16);
        if (i == 0 && offset > pos) {
            Segment gap;
            gap.begin = pos;
            gap.end = offset;
            segs.push_back(gap);
            pos = offset;
        }
        if (offset != pos)
            fail(Fail::Input, "The compressed dump's section table does not match the NCA layout (section %llu starts at "
                              "%llu, expected %llu). The dump is damaged.",
                 static_cast<unsigned long long>(i), static_cast<unsigned long long>(offset),
                 static_cast<unsigned long long>(pos));
        if (size > ~u64(0) - pos)
            fail(Fail::Input, "The compressed dump's section %llu is too large. The dump is damaged.",
                 static_cast<unsigned long long>(i));
        if (size != 0) {
            Segment s;
            s.begin = pos;
            s.end = pos + size;
            s.ctr = (type == 3 || type == 4);   // AES-128-CTR; other types are stored plain, as nsz does
            if (s.ctr) {
                std::memcpy(s.key, e + 32, 16);
                std::memcpy(s.nonce, e + 48, 16);
            }
            segs.push_back(s);
        }
        pos += size;
    }
    const u64 size = pos;

    bool blocks = false;
    if (file - table_end >= 8) {
        u8 magic[8];
        ncz->read(table_end, magic, sizeof magic);
        blocks = std::memcmp(magic, "NCZBLOCK", 8) == 0;
    }
    return std::make_shared<NczStorage>(std::move(ncz), std::move(segs), size, table_end, blocks);
}

#endif

}  // namespace mprs
