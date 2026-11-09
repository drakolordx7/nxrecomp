// SPDX-License-Identifier: GPL-3.0-or-later
#include "nca.h"

#include <algorithm>
#include <cstring>

#include "bktr.h"
#include "crypto.h"
#include "log.h"

namespace nxs {
namespace {

u32 rd32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u64 rd64(const u8* p) { return u64(rd32(p)) | u64(rd32(p + 4)) << 32; }

constexpr u64 kNcaHeaderSize = 0xC00;

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
        const u8* hd = s.header.data();
        s.meta_hash_type = hd[5];
        if (std::memcmp(hd + 0x110, "BKTR", 4) == 0) {
            s.indirect_off = rd64(hd + 0x100);
            s.indirect_size = rd64(hd + 0x108);
            s.indirect_count = rd32(hd + 0x118);
        }
        if (std::memcmp(hd + 0x130, "BKTR", 4) == 0) {
            s.aes_ctr_ex_off = rd64(hd + 0x120);
            s.aes_ctr_ex_size = rd64(hd + 0x128);
            s.aes_ctr_ex_count = rd32(hd + 0x138);
        }
        s.sparse_generation = static_cast<u16>(hd[0x170] | hd[0x171] << 8);
        s.compression_size = rd64(hd + 0x180);
        if (s.indirect_off > s.size || s.indirect_size > s.size - s.indirect_off || s.aes_ctr_ex_off > s.size ||
            s.aes_ctr_ex_size > s.size - s.aes_ctr_ex_off)
            fail(Fail::Input, "An update table in the dump points outside its section.");
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

StoragePtr Nca::open_section(int i, const Nca* base) const {
    const NcaSection* s = section(i);
    if (!s) fail(Fail::Input, "The dump has no section %d in an archive where one is expected.", i);
    if (s->sparse_generation != 0)
        fail(Fail::Input, "An archive in the dump is stored sparse (a layout this setup does not read yet).");
    if (s->compression_size != 0)
        fail(Fail::Input, "An archive in the dump is compressed inside (a layout this setup does not read yet).");
    StoragePtr raw = make_sub_storage(file_, s->offset, s->size);
    if (s->enc_type != 1 && s->enc_type != 3 && s->enc_type != 4)
        fail(Fail::Input, "An archive in the dump uses an encryption mode (%u) that is not supported.", (unsigned)s->enc_type);
    if (!s->is_patch()) {
        if (s->enc_type == 1) return raw;
        if (s->enc_type == 3) return make_aes_ctr_storage(raw, section_key(*s), s->ctr_hi, s->offset);
        fail(Fail::Input, "An archive in the dump is an update section without its patch table.");
    }
    if (!base) {
        fail(Fail::Input, "This is a game update (patch). Its data is a change to the base game, so setup needs the base game "
                          "dump too (give it as another input).");
    }
    const int brs = base->romfs_section();
    if (brs < 0) fail(Fail::Input, "The base game has no RomFS section for the update to apply to.");
    const Key128 key = s->enc_type == 1 ? Key128{} : section_key(*s);
    // The patch tables use the section's own counter; the data regions use the AES-CTR-EX entries.
    StoragePtr plain = s->enc_type == 1 ? raw : make_aes_ctr_storage(raw, key, s->ctr_hi, s->offset);
    if (s->indirect_size > kMaxBucketTableSize || s->aes_ctr_ex_size > kMaxBucketTableSize)
        fail(Fail::Input, "The update's patch tables are larger than this setup reads (damaged, or a layout not supported yet).");
    u64 ind_end = 0;
    std::vector<IndirectEntry> ind =
        parse_indirect_table(read_all(*plain, s->indirect_off, static_cast<size_t>(s->indirect_size)), s->indirect_count, ind_end);
    StoragePtr data = plain;
    if (s->enc_type == 4) {
        if (s->aes_ctr_ex_size == 0) fail(Fail::Input, "The update has no encryption table.");
        u64 ex_end = 0;
        std::vector<AesCtrExEntry> ex = parse_aes_ctr_ex_table(
            read_all(*plain, s->aes_ctr_ex_off, static_cast<size_t>(s->aes_ctr_ex_size)), s->aes_ctr_ex_count, ex_end);
        data = make_aes_ctr_ex_storage(raw, key, s->ctr_hi, s->offset, std::move(ex), ex_end);
    }
    return make_indirect_storage(std::move(ind), ind_end, base->open_section(brs), data);
}

int Nca::romfs_section() const {
    for (int i = 0; i < 4; ++i)
        if (const NcaSection* s = section(i))
            if (s->fs_type == 0 && s->hash_type == 3) return i;
    return -1;
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
// limit = the size of the opened section (an updated section is larger than the update's own bytes).
Ivfc parse_ivfc(const NcaSection& s, u64 limit) {
    const u8* hd = s.header.data() + 8;
    if (std::memcmp(hd, "IVFC", 4) != 0) fail(Fail::Input, "The RomFS section of the dump has no integrity tree header.");
    Ivfc t;
    u32 nlev = rd32(hd + 0xC);
    if (nlev < 2 || nlev > 7) fail(Fail::Input, "The RomFS integrity tree has an unexpected shape.");
    for (u32 i = 0; i + 1 < nlev; ++i) {
        const u8* e = hd + 0x10 + 0x18 * i;
        IvfcLevel l{rd64(e), rd64(e + 8), rd32(e + 16)};
        if (l.bs < 9 || l.bs > 24 || l.off > limit || l.size > limit - l.off)
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

StoragePtr Nca::open_romfs_image(int i, const Nca* base) const {
    const NcaSection* s = section(i);
    if (!s || s->fs_type != 0 || s->hash_type != 3)
        fail(Fail::Input, "The dump does not have the expected RomFS section.");
    StoragePtr sec = open_section(i, base);
    Ivfc t = parse_ivfc(*s, sec->size());
    const IvfcLevel& d = t.level.back();
    return make_sub_storage(sec, d.off, d.size);
}

u64 Nca::verify_section(int i, const ProgressFn& progress, const CancelFn& cancelled, const Nca* base) const {
    const NcaSection* s = section(i);
    if (!s) fail(Fail::Input, "The dump has no section %d to check.", i);
    StoragePtr sec = open_section(i, base);
    u64 hashed = 0;
    auto check_cancel = [&] {
        if (cancelled && cancelled()) fail(Fail::Cancelled, "Stopped by the user.");
    };

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
        Ivfc t = parse_ivfc(*s, sec->size());
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

}  // namespace nxs
