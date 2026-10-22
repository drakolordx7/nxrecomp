// SPDX-License-Identifier: GPL-3.0-or-later
#include "nca.h"

#include <algorithm>
#include <cstring>

#include "crypto.h"
#include "log.h"

namespace mprs {
namespace {

u32 rd32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u64 rd64(const u8* p) { return u64(rd32(p)) | u64(rd32(p + 4)) << 32; }

constexpr u64 kNcaHeaderSize = 0xC00;

// AES-128-CTR over an encrypted section. The counter is ctr_hi (8 bytes) followed by the big-endian 64-bit value
// (absolute byte offset inside the NCA file) / 16.
class AesCtrStorage final : public IStorage {
public:
    AesCtrStorage(StoragePtr base, const Key128& key, const std::array<u8, 8>& hi, u64 nca_offset)
        : base_(std::move(base)), key_(key), hi_(hi), nca_off_(nca_offset) {}
    u64 size() const override { return base_->size(); }
    void read(u64 off, void* buf, size_t len) const override {
        if (len == 0) return;
        u64 abs = nca_off_ + off;
        u64 a = abs & ~u64(15);
        size_t extra = static_cast<size_t>(abs - a);
        u8 ctr[16];
        std::memcpy(ctr, hi_.data(), 8);
        u64 blk = a >> 4;
        for (int i = 0; i < 8; ++i) ctr[8 + i] = static_cast<u8>(blk >> (56 - 8 * i));
        if (extra == 0) {
            base_->read(off, buf, len);
            aes_ctr_crypt(key_.data(), ctr, buf, len);
        } else {
            std::vector<u8> tmp(extra + len);
            // bytes before `off` inside the 16-byte block are not needed from storage; zero them (their key stream is discarded)
            base_->read(off, tmp.data() + extra, len);
            aes_ctr_crypt(key_.data(), ctr, tmp.data(), tmp.size());
            std::memcpy(buf, tmp.data() + extra, len);
        }
    }
private:
    StoragePtr base_;
    Key128 key_;
    std::array<u8, 8> hi_;
    u64 nca_off_;
};

std::string rights_hex(const std::array<u8, 16>& r) { return hex_encode(r.data(), 16); }

// Hash of one block of a hash-tree level, over `len` bytes (the caller retries a short final block zero-padded).
std::array<u8, 32> hash_block(const u8* p, size_t len) { return sha256(p, len); }

}  // namespace

std::unique_ptr<Nca> Nca::open(StoragePtr file, const KeySet& keys, const TicketKeys& tickets) {
    if (file->size() < kNcaHeaderSize) fail(Fail::Input, "A file inside the dump is too small to be a game archive (NCA).");
    std::vector<u8> raw = read_all(*file, 0, kNcaHeaderSize);
    std::vector<u8> h = raw;
    const std::array<u8, 32> hk = keys.header_key();
    aes_xts_decrypt_nintendo(hk.data(), hk.data() + 16, 0, 0x200, h.data(), h.size());
    if (std::memcmp(h.data() + 0x200, "NCA", 3) != 0 || (h[0x203] != '3' && h[0x203] != '2' && h[0x203] != '0'))
        fail(Fail::Keys, "The keys cannot open this dump (the archive header does not decrypt). The keys file may be too old for "
                         "this game, or it is not from a Switch.");
    if (h[0x203] == '0') fail(Fail::Input, "This dump uses an obsolete archive format (NCA0) that is not supported.");
    if (h[0x203] == '2') {
        // NCA2: every section header is its own XTS unit starting at sector 0
        for (int i = 0; i < 4; ++i) {
            std::memcpy(h.data() + 0x400 + 0x200 * i, raw.data() + 0x400 + 0x200 * i, 0x200);
            aes_xts_decrypt_nintendo(hk.data(), hk.data() + 16, 0, 0x200, h.data() + 0x400 + 0x200 * i, 0x200);
        }
    }

    auto n = std::unique_ptr<Nca>(new Nca());
    n->file_ = std::move(file);
    n->keys_ = &keys;
    n->tickets_ = tickets;
    if (h[0x205] > 5) fail(Fail::Input, "An archive in the dump has an unknown content type.");
    n->type_ = static_cast<NcaContentType>(h[0x205]);
    n->key_area_kind_ = h[0x207];
    n->content_size_ = rd64(&h[0x208]);
    n->program_id_ = rd64(&h[0x210]);
    n->content_index_ = rd32(&h[0x218]);
    int gen = std::max<int>(h[0x206], h[0x220]);
    n->key_gen_ = gen > 0 ? gen - 1 : 0;
    std::memcpy(n->rights_id_.data(), &h[0x230], 16);
    n->has_rights_ = std::any_of(n->rights_id_.begin(), n->rights_id_.end(), [](u8 b) { return b != 0; });
    std::memcpy(n->key_area_.data(), &h[0x300], 0x40);

    for (int i = 0; i < 4; ++i) {
        const u8* e = &h[0x240 + 0x10 * i];
        u64 start = u64(rd32(e)) * 0x200, end = u64(rd32(e + 4)) * 0x200;
        if (end <= start) continue;
        NcaSection& s = n->sec_[i];
        s.index = i;
        s.offset = start;
        s.size = end - start;
        if (end > n->file_->size())
            fail(Fail::Input, "The dump is cut short: an archive ends before its last section (needs %llu bytes, has %llu). "
                              "The file was probably not copied completely.",
                 (unsigned long long)end, (unsigned long long)n->file_->size());
        std::memcpy(s.header.data(), &h[0x400 + 0x200 * i], 0x200);
        s.fs_type = s.header[2];
        s.hash_type = s.header[3];
        s.enc_type = s.header[4];
        for (int k = 0; k < 8; ++k) s.ctr_hi[k] = s.header[0x140 + 7 - k];
    }
    return n;
}

Key128 Nca::section_key(const NcaSection& s) const {
    if (has_rights_) {
        const std::string r = rights_hex(rights_id_);
        Key128 enc{};
        auto it = tickets_.common.find(r);
        if (it != tickets_.common.end()) enc = it->second;
        else if (!keys_->title_key(rights_id_, enc)) {
            if (std::find(tickets_.personalized.begin(), tickets_.personalized.end(), r) != tickets_.personalized.end())
                fail(Fail::Keys, "This dump's ticket is tied to one Switch console. Add that console's title key to a title.keys "
                                 "file next to your prod.keys (rights id %s).", r.c_str());
            fail(Fail::Keys, "This dump has no ticket and your keys have no title key for it (rights id %s). Use the NSP with its "
                             ".tik file, or add the key to title.keys.", r.c_str());
        }
        Aes128Ecb ecb(keys_->titlekek(key_gen_).data());
        Key128 out;
        ecb.decrypt(enc.data(), out.data(), 16);
        return out;
    }
    if (key_area_kind_ > 2) fail(Fail::Input, "An archive in the dump uses an unknown key area.");
    Key128 kak = keys_->key_area_key(key_area_kind_, key_gen_);
    Aes128Ecb ecb(kak.data());
    u8 area[0x40];
    ecb.decrypt(key_area_.data(), area, 0x40);
    Key128 out;
    std::memcpy(out.data(), area + 0x20, 16);   // key area entry 2 = the AES-CTR key
    (void)s;
    return out;
}

StoragePtr Nca::open_section(int i) const {
    const NcaSection* s = section(i);
    if (!s) fail(Fail::Input, "The dump has no section %d in an archive where one is expected.", i);
    StoragePtr raw = make_sub_storage(file_, s->offset, s->size);
    if (s->enc_type == 1) return raw;
    if (s->enc_type == 3) return std::make_shared<AesCtrStorage>(raw, section_key(*s), s->ctr_hi, s->offset);
    if (s->enc_type == 4)
        fail(Fail::Input, "This is a game update (patch) file. Only the original v1.0.0 game dump is supported.");
    fail(Fail::Input, "An archive in the dump uses an encryption mode (%u) that is not supported.", (unsigned)s->enc_type);
}

namespace {
struct Sha256Layers {
    std::array<u8, 32> master{};
    u32 block_size = 0;
    u64 table_off = 0, table_size = 0, data_off = 0, data_size = 0;
};
Sha256Layers parse_sha256_layers(const NcaSection& s) {
    const u8* hd = s.header.data() + 8;
    Sha256Layers l;
    std::memcpy(l.master.data(), hd, 32);
    l.block_size = rd32(hd + 0x20);
    u32 layers = rd32(hd + 0x24);
    if (layers != 2 || l.block_size == 0 || (l.block_size & (l.block_size - 1)))
        fail(Fail::Input, "A file system in the dump has an unexpected hash layout.");
    l.table_off = rd64(hd + 0x28);
    l.table_size = rd64(hd + 0x30);
    l.data_off = rd64(hd + 0x38);
    l.data_size = rd64(hd + 0x40);
    if (l.table_off > s.size || l.table_size > s.size - l.table_off || l.data_off > s.size || l.data_size > s.size - l.data_off)
        fail(Fail::Input, "A file system in the dump points outside its section.");
    return l;
}
struct IvfcLevel { u64 off, size; u32 bs; };
struct Ivfc {
    std::array<u8, 32> master{};
    std::vector<IvfcLevel> level;
};
Ivfc parse_ivfc(const NcaSection& s) {
    const u8* hd = s.header.data() + 8;
    if (std::memcmp(hd, "IVFC", 4) != 0) fail(Fail::Input, "The RomFS section of the dump has no integrity tree header.");
    Ivfc t;
    u32 nlev = rd32(hd + 0xC);
    if (nlev < 2 || nlev > 7) fail(Fail::Input, "The RomFS integrity tree has an unexpected shape.");
    for (u32 i = 0; i + 1 < nlev; ++i) {
        const u8* e = hd + 0x10 + 0x18 * i;
        IvfcLevel l{rd64(e), rd64(e + 8), rd32(e + 16)};
        if (l.bs < 9 || l.bs > 24 || l.off > s.size || l.size > s.size - l.off)
            fail(Fail::Input, "The RomFS integrity tree points outside its section.");
        t.level.push_back(l);
    }
    std::memcpy(t.master.data(), hd + 0xC0, 32);
    return t;
}
}  // namespace

std::unique_ptr<Pfs> Nca::open_pfs0(int i) const {
    const NcaSection* s = section(i);
    if (!s || s->fs_type != 1 || s->hash_type != 2)
        fail(Fail::Input, "The dump does not have the expected executable file system (ExeFS).");
    Sha256Layers l = parse_sha256_layers(*s);
    StoragePtr sec = open_section(i);
    return Pfs::open(make_sub_storage(sec, l.data_off, l.data_size));
}

StoragePtr Nca::open_romfs_image(int i) const {
    const NcaSection* s = section(i);
    if (!s || s->fs_type != 0 || s->hash_type != 3)
        fail(Fail::Input, "The dump does not have the expected RomFS section.");
    Ivfc t = parse_ivfc(*s);
    StoragePtr sec = open_section(i);
    const IvfcLevel& d = t.level.back();
    return make_sub_storage(sec, d.off, d.size);
}

u64 Nca::verify_section(int i, const ProgressFn& progress, const CancelFn& cancelled) const {
    const NcaSection* s = section(i);
    if (!s) fail(Fail::Input, "The dump has no section %d to check.", i);
    StoragePtr sec = open_section(i);
    u64 hashed = 0;
    auto check_cancel = [&] {
        if (cancelled && cancelled()) fail(Fail::Cancelled, "Stopped by the user.");
    };

    // Verifies `data_size` bytes of `data` (at data_off) in blocks of `bs` against 32-byte hashes in `hashes`.
    auto verify_level = [&](const char* what, u64 data_off, u64 data_size, u32 bs, const std::vector<u8>& hashes, u64 total_for_progress) {
        const u64 nblocks = (data_size + bs - 1) / bs;
        if (hashes.size() < nblocks * 32) fail(Fail::Input, "The %s of the dump is damaged: hash list too short.", what);
        const u64 chunk_blocks = std::max<u64>(1, (4u << 20) / bs);
        std::vector<u8> buf;
        for (u64 b = 0; b < nblocks; b += chunk_blocks) {
            check_cancel();
            u64 nb = std::min<u64>(chunk_blocks, nblocks - b);
            u64 off = b * bs;
            u64 len = std::min<u64>(nb * bs, data_size - off);
            buf.resize(static_cast<size_t>(len));
            sec->read(data_off + off, buf.data(), static_cast<size_t>(len));
            for (u64 k = 0; k < nb; ++k) {
                size_t o = static_cast<size_t>(k * bs);
                size_t l = static_cast<size_t>(std::min<u64>(bs, len - o));
                const u8* expect = hashes.data() + (b + k) * 32;
                auto hsh = hash_block(buf.data() + o, l);
                bool good = std::memcmp(hsh.data(), expect, 32) == 0;
                if (!good && l < bs) {
                    // the integrity tree of RomFS hashes a short final block as if it were zero-padded to the block size
                    std::vector<u8> pad(bs, 0);
                    std::memcpy(pad.data(), buf.data() + o, l);
                    auto h2 = hash_block(pad.data(), bs);
                    good = std::memcmp(h2.data(), expect, 32) == 0;
                }
                if (!good)
                    fail(Fail::Input, "The dump is damaged: the %s fails its check at about %.2f GB. The file was probably not "
                                      "copied correctly (run a file check on it, or dump the game again).",
                         what, double(data_off + off + k * bs) / 1e9);
            }
            hashed += len;
            if (progress) progress(hashed, total_for_progress);
        }
    };

    if (s->hash_type == 2) {
        Sha256Layers l = parse_sha256_layers(*s);
        std::vector<u8> table = read_all(*sec, l.table_off, static_cast<size_t>(l.table_size));
        auto m = sha256(table.data(), table.size());
        if (m != l.master) fail(Fail::Input, "The dump is damaged: the executable file system table fails its check.");
        verify_level("executable file system", l.data_off, l.data_size, l.block_size, table, l.data_size);
        return hashed;
    }
    if (s->hash_type == 3) {
        Ivfc t = parse_ivfc(*s);
        // level 0 against the master hash (one block covering the whole level)
        {
            std::vector<u8> l0 = read_all(*sec, t.level[0].off, static_cast<size_t>(t.level[0].size));
            auto m = sha256(l0.data(), l0.size());
            if (m != t.master) fail(Fail::Input, "The dump is damaged: the RomFS integrity tree fails its top-level check.");
        }
        u64 total = 0;
        for (const IvfcLevel& l : t.level) total += l.size;
        for (size_t lv = 1; lv < t.level.size(); ++lv) {
            const IvfcLevel& up = t.level[lv - 1];
            const IvfcLevel& cur = t.level[lv];
            std::vector<u8> hashes = read_all(*sec, up.off, static_cast<size_t>(up.size));
            verify_level(lv + 1 == t.level.size() ? "RomFS data" : "RomFS integrity tree", cur.off, cur.size, 1u << cur.bs, hashes,
                         total);
        }
        return hashed;
    }
    fail(Fail::Input, "A file system in the dump uses a hash scheme (%u) that is not supported.", (unsigned)s->hash_type);
}

}  // namespace mprs
