// SPDX-License-Identifier: GPL-3.0-or-later
#include "bktr.h"

#include <algorithm>
#include <cstring>

#include "crypto.h"

namespace mprs {
namespace {

u32 rd32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u64 rd64(const u8* p) { return u64(rd32(p)) | u64(rd32(p + 4)) << 32; }

// Walks the entry sets of a bucket tree and hands each entry's bytes to `take`. Layout: node 0 is the L1 node {index, set count,
// end offset, set start offsets...}; entry set k is node 1 + k {index k, entry count, end offset, entries...}.
template <class Take>
void walk_bucket_tree(const std::vector<u8>& t, u32 entry_count, size_t entry_size, const char* what, u64& end_offset, Take take) {
    auto bad = [&](const char* why) { fail(Fail::Input, "The update's %s table is damaged (%s).", what, why); };
    if (entry_count == 0) bad("no entries");
    const u64 per_set = (kBucketNodeSize - 0x10) / entry_size;
    const u64 need_sets = (u64(entry_count) + per_set - 1) / per_set;
    if (need_sets > (kBucketNodeSize - 0x10) / 8)
        fail(Fail::Input, "The update's %s table uses a layout this setup does not read yet (more than one index level).", what);
    if (t.size() < kBucketNodeSize) bad("too short");
    const u32 sets = rd32(t.data() + 4);
    end_offset = rd64(t.data() + 8);
    if (sets == 0) bad("no entry sets");
    if (sets != need_sets) bad("entry set count does not match");
    if (t.size() / kBucketNodeSize < 1 + u64(sets)) bad("entry sets missing");
    u64 total = 0;
    for (u32 k = 0; k < sets; ++k) {
        const u8* n = t.data() + kBucketNodeSize * (1 + k);
        const u32 idx = rd32(n), cnt = rd32(n + 4);
        if (idx != k || cnt == 0 || cnt > per_set) bad("bad entry set header");
        const u64 set_end = rd64(n + 8);
        const u64 set_start = rd64(t.data() + 0x10 + 8 * k);
        for (u32 e = 0; e < cnt; ++e) take(n + 0x10 + entry_size * e, e == 0 ? &set_start : nullptr, set_end);
        total += cnt;
    }
    if (total != entry_count) bad("entry count does not match");
}

void check_sorted_offsets(const std::vector<u64>& offs, u64 end, const char* what, bool from_zero) {
    if (offs.empty() || (from_zero && offs[0] != 0))
        fail(Fail::Input, "The update's %s table is damaged (it does not start at 0).", what);
    for (size_t i = 1; i < offs.size(); ++i)
        if (offs[i] <= offs[i - 1]) fail(Fail::Input, "The update's %s table is damaged (entries out of order).", what);
    if (end <= offs.back()) fail(Fail::Input, "The update's %s table is damaged (bad end offset).", what);
}

// Reads len bytes at `off` of raw and decrypts them with AES-CTR (counter for absolute offset abs_off of the first byte).
void ctr_read(const IStorage& raw, const Key128& key, const u8 hi[8], u64 abs_off, u64 off, u8* buf, size_t len) {
    if (len == 0) return;
    const u64 a = abs_off & ~u64(15);
    const size_t extra = static_cast<size_t>(abs_off - a);
    u8 ctr[16];
    std::memcpy(ctr, hi, 8);
    const u64 blk = a >> 4;
    for (int i = 0; i < 8; ++i) ctr[8 + i] = static_cast<u8>(blk >> (56 - 8 * i));
    if (extra == 0) {
        raw.read(off, buf, len);
        aes_ctr_crypt(key.data(), ctr, buf, len);
    } else {
        std::vector<u8> tmp(extra + len);   // the leading bytes only position the key stream
        raw.read(off, tmp.data() + extra, len);
        aes_ctr_crypt(key.data(), ctr, tmp.data(), tmp.size());
        std::memcpy(buf, tmp.data() + extra, len);
    }
}

class AesCtrStorage final : public IStorage {
public:
    AesCtrStorage(StoragePtr raw, const Key128& key, const std::array<u8, 8>& hi, u64 nca_off)
        : raw_(std::move(raw)), key_(key), hi_(hi), nca_off_(nca_off) {}
    u64 size() const override { return raw_->size(); }
    void read(u64 off, void* buf, size_t len) const override {
        ctr_read(*raw_, key_, hi_.data(), nca_off_ + off, off, static_cast<u8*>(buf), len);
    }
private:
    StoragePtr raw_;
    Key128 key_;
    std::array<u8, 8> hi_;
    u64 nca_off_;
};

class AesCtrExStorage final : public IStorage {
public:
    AesCtrExStorage(StoragePtr raw, const Key128& key, const std::array<u8, 8>& hi, u64 nca_off, std::vector<AesCtrExEntry> e, u64 end)
        : raw_(std::move(raw)), key_(key), hi_(hi), nca_off_(nca_off), e_(std::move(e)), end_(end) {}
    u64 size() const override { return raw_->size(); }
    void read(u64 off, void* buf, size_t len) const override {
        if (len == 0) return;
        u8* p = static_cast<u8*>(buf);
        if (off < e_.front().offset || off > end_ || len > end_ - off)
            fail(Fail::Input, "The update points outside its encrypted data (offset %llu, %llu bytes).", (unsigned long long)off,
                 (unsigned long long)len);
        auto it = std::upper_bound(e_.begin(), e_.end(), off, [](u64 v, const AesCtrExEntry& x) { return v < x.offset; }) - 1;
        while (len > 0) {
            const u64 stop = (it + 1 == e_.end()) ? end_ : (it + 1)->offset;
            const size_t n = static_cast<size_t>(std::min<u64>(len, stop - off));
            if (it->encryption == 1) {
                raw_->read(off, p, n);
            } else {
                u8 hi[8];
                std::memcpy(hi, hi_.data(), 8);
                for (int i = 0; i < 4; ++i) hi[4 + i] = static_cast<u8>(it->generation >> (24 - 8 * i));
                ctr_read(*raw_, key_, hi, nca_off_ + off, off, p, n);
            }
            p += n;
            off += n;
            len -= n;
            ++it;
        }
    }
private:
    StoragePtr raw_;
    Key128 key_;
    std::array<u8, 8> hi_;
    u64 nca_off_;
    std::vector<AesCtrExEntry> e_;
    u64 end_;
};

class IndirectStorage final : public IStorage {
public:
    IndirectStorage(std::vector<IndirectEntry> e, u64 size, StoragePtr base, StoragePtr patch)
        : e_(std::move(e)), size_(size), src_{std::move(base), std::move(patch)} {}
    u64 size() const override { return size_; }
    void read(u64 off, void* buf, size_t len) const override {
        if (off > size_ || len > size_ - off)
            fail(Fail::Input, "A read goes past the end of the updated game data (offset %llu, %llu bytes).", (unsigned long long)off,
                 (unsigned long long)len);
        if (len == 0) return;
        u8* p = static_cast<u8*>(buf);
        auto it = std::upper_bound(e_.begin(), e_.end(), off, [](u64 v, const IndirectEntry& x) { return v < x.virt; }) - 1;
        while (len > 0) {
            const u64 stop = (it + 1 == e_.end()) ? size_ : (it + 1)->virt;
            const size_t n = static_cast<size_t>(std::min<u64>(len, stop - off));
            src_[it->storage]->read(it->phys + (off - it->virt), p, n);
            p += n;
            off += n;
            len -= n;
            ++it;
        }
    }
private:
    std::vector<IndirectEntry> e_;
    u64 size_;
    StoragePtr src_[2];
};

}  // namespace

std::vector<IndirectEntry> parse_indirect_table(const std::vector<u8>& table, u32 entry_count, u64& end_offset) {
    std::vector<IndirectEntry> out;
    out.reserve(entry_count);
    walk_bucket_tree(table, entry_count, 0x14, "patch index", end_offset, [&](const u8* p, const u64*, u64) {
        IndirectEntry e{rd64(p), rd64(p + 8), rd32(p + 16)};
        if (e.storage > 1) fail(Fail::Input, "The update's patch index table names an unknown data source (%u).", e.storage);
        out.push_back(e);
    });
    std::vector<u64> offs;
    for (auto& e : out) offs.push_back(e.virt);
    check_sorted_offsets(offs, end_offset, "patch index", true);
    return out;
}

std::vector<AesCtrExEntry> parse_aes_ctr_ex_table(const std::vector<u8>& table, u32 entry_count, u64& end_offset) {
    std::vector<AesCtrExEntry> out;
    out.reserve(entry_count);
    walk_bucket_tree(table, entry_count, 0x10, "encryption", end_offset, [&](const u8* p, const u64*, u64) {
        out.push_back(AesCtrExEntry{rd64(p), p[8], rd32(p + 12)});
    });
    std::vector<u64> offs;
    for (auto& e : out) offs.push_back(e.offset);
    check_sorted_offsets(offs, end_offset, "encryption", false);
    return out;
}

StoragePtr make_aes_ctr_storage(StoragePtr raw, const Key128& key, const std::array<u8, 8>& ctr_hi, u64 nca_offset) {
    return std::make_shared<AesCtrStorage>(std::move(raw), key, ctr_hi, nca_offset);
}

StoragePtr make_aes_ctr_ex_storage(StoragePtr raw, const Key128& key, const std::array<u8, 8>& ctr_hi, u64 nca_offset,
                                   std::vector<AesCtrExEntry> entries, u64 end_offset) {
    if (entries.empty() || end_offset > raw->size())
        fail(Fail::Input, "The update's encryption table points outside its section.");
    return std::make_shared<AesCtrExStorage>(std::move(raw), key, ctr_hi, nca_offset, std::move(entries), end_offset);
}

StoragePtr make_indirect_storage(std::vector<IndirectEntry> entries, u64 size, StoragePtr base, StoragePtr patch) {
    if (entries.empty() || entries[0].virt != 0) fail(Fail::Input, "The update's patch index table is empty or damaged.");
    return std::make_shared<IndirectStorage>(std::move(entries), size, std::move(base), std::move(patch));
}

}  // namespace mprs
