// SPDX-License-Identifier: GPL-3.0-or-later
// Builders for synthetic Switch dumps. See synth_nca.h. Layout facts come from src/nca.cpp, src/container.cpp, src/keys.cpp
// (and switchbrew for the ticket rights id offset, which the reader does not follow; see the test report).
#include "synth_nca.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "synth.h"

namespace mprs_test {
namespace {

// ---- AES-128 (independent implementation: S-box computed from GF(2^8) inversion) ---------------------------------------------

uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    while (b) {
        if (b & 1) p ^= a;
        const uint8_t hi = a & 0x80;
        a = uint8_t(a << 1);
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

uint8_t ginv(uint8_t a) {
    if (a == 0) return 0;
    uint8_t res = 1, base = a;
    int e = 254;
    while (e) {
        if (e & 1) res = gmul(res, base);
        base = gmul(base, base);
        e >>= 1;
    }
    return res;
}

uint8_t rotl8(uint8_t b, unsigned n) { return uint8_t((b << n) | (b >> (8 - n))); }

struct AesTables {
    uint8_t s[256];
    uint8_t si[256];
    AesTables() {
        for (int x = 0; x < 256; ++x) {
            const uint8_t b = ginv(uint8_t(x));
            s[x] = uint8_t(b ^ rotl8(b, 1) ^ rotl8(b, 2) ^ rotl8(b, 3) ^ rotl8(b, 4) ^ 0x63);
        }
        for (int x = 0; x < 256; ++x) si[s[x]] = uint8_t(x);
    }
};

const AesTables& tables() {
    static const AesTables t;
    return t;
}

class Aes128 {
public:
    explicit Aes128(const uint8_t key[16]) {
        const AesTables& t = tables();
        std::memcpy(rk_, key, 16);
        uint8_t rcon = 1;
        for (int i = 16; i < 176; i += 4) {
            uint8_t w[4] = {rk_[i - 4], rk_[i - 3], rk_[i - 2], rk_[i - 1]};
            if (i % 16 == 0) {
                const uint8_t w0 = w[0];
                w[0] = uint8_t(t.s[w[1]] ^ rcon);
                w[1] = t.s[w[2]];
                w[2] = t.s[w[3]];
                w[3] = t.s[w0];
                rcon = gmul(rcon, 2);
            }
            for (int j = 0; j < 4; ++j) rk_[i + j] = uint8_t(rk_[i - 16 + j] ^ w[j]);
        }
    }

    void encrypt(uint8_t* s) const {
        const AesTables& t = tables();
        add(s, rk_);
        for (int r = 1; r <= 10; ++r) {
            for (int i = 0; i < 16; ++i) s[i] = t.s[s[i]];
            shift_rows(s, false);
            if (r < 10) mix_columns(s, false);
            add(s, rk_ + 16 * r);
        }
    }

    void decrypt(uint8_t* s) const {
        const AesTables& t = tables();
        add(s, rk_ + 160);
        for (int r = 9; r >= 0; --r) {
            shift_rows(s, true);
            for (int i = 0; i < 16; ++i) s[i] = t.si[s[i]];
            add(s, rk_ + 16 * r);
            if (r > 0) mix_columns(s, true);
        }
    }

private:
    static void add(uint8_t* s, const uint8_t* k) {
        for (int i = 0; i < 16; ++i) s[i] ^= k[i];
    }
    // State byte index = row + 4*column; row r moves left by r columns (right when inverse).
    static void shift_rows(uint8_t* s, bool inverse) {
        uint8_t t[16];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                const int src_c = inverse ? (c + 4 - r) % 4 : (c + r) % 4;
                t[r + 4 * c] = s[r + 4 * src_c];
            }
        std::memcpy(s, t, 16);
    }
    static void mix_columns(uint8_t* s, bool inverse) {
        for (int c = 0; c < 4; ++c) {
            uint8_t* a = s + 4 * c;
            const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
            if (!inverse) {
                a[0] = uint8_t(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
                a[1] = uint8_t(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
                a[2] = uint8_t(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
                a[3] = uint8_t(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
            } else {
                a[0] = uint8_t(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                a[1] = uint8_t(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                a[2] = uint8_t(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                a[3] = uint8_t(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
    }
    uint8_t rk_[176];
};

void ecb_encrypt(const uint8_t key[16], uint8_t* data, size_t n) {
    Aes128 a(key);
    for (size_t i = 0; i < n; i += 16) a.encrypt(data + i);
}
void ecb_decrypt(const uint8_t key[16], uint8_t* data, size_t n) {
    Aes128 a(key);
    for (size_t i = 0; i < n; i += 16) a.decrypt(data + i);
}

// AES-CTR: the 16-byte counter block is incremented as a big-endian integer once per block.
void ctr_crypt(const uint8_t key[16], const uint8_t ctr0[16], uint8_t* data, size_t len) {
    Aes128 a(key);
    uint8_t ctr[16];
    std::memcpy(ctr, ctr0, 16);
    for (size_t off = 0; off < len; off += 16) {
        uint8_t ks[16];
        std::memcpy(ks, ctr, 16);
        a.encrypt(ks);
        const size_t n = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < n; ++i) data[off + i] ^= ks[i];
        for (int i = 15; i >= 0; --i)
            if (++ctr[i] != 0) break;
    }
}

// Nintendo XTS: standard XTS with the sector number as a big-endian 64-bit value in bytes 8..15 of the tweak input.
void gf_mul_x(uint8_t t[16]) {
    uint8_t carry = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t next = uint8_t(t[i] >> 7);
        t[i] = uint8_t((t[i] << 1) | carry);
        carry = next;
    }
    if (carry) t[0] ^= 0x87;
}
void xts(const uint8_t k1[16], const uint8_t k2[16], uint64_t sector0, uint32_t sector_size, uint8_t* data, size_t len, bool enc) {
    Aes128 data_key(k1), tweak_key(k2);
    const size_t sectors = len / sector_size;
    for (size_t s = 0; s < sectors; ++s) {
        uint8_t t[16] = {};
        const uint64_t sec = sector0 + s;
        for (int i = 0; i < 8; ++i) t[8 + i] = uint8_t(sec >> (56 - 8 * i));
        tweak_key.encrypt(t);
        uint8_t* p = data + s * sector_size;
        for (uint32_t j = 0; j < sector_size; j += 16) {
            uint8_t blk[16];
            for (int i = 0; i < 16; ++i) blk[i] = uint8_t(p[j + i] ^ t[i]);
            if (enc) data_key.encrypt(blk);
            else data_key.decrypt(blk);
            for (int i = 0; i < 16; ++i) p[j + i] = uint8_t(blk[i] ^ t[i]);
            gf_mul_x(t);
        }
    }
}

// ---- small helpers ----------------------------------------------------------------------------------------------------------

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint64_t roundup(uint64_t x, uint64_t a) { return (x + a - 1) / a * a; }

void fill_seeded(uint8_t* p, size_t n, uint32_t seed) {
    uint32_t x = seed * 2654435761u + 12345u;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;
        p[i] = uint8_t(x >> 16);
    }
}
Bytes16 seeded16(uint32_t seed) {
    Bytes16 b;
    fill_seeded(b.data(), 16, seed);
    return b;
}

std::string hex_str(const uint8_t* p, size_t n, bool upper) {
    static const char* lo = "0123456789abcdef";
    static const char* up = "0123456789ABCDEF";
    const char* d = upper ? up : lo;
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}
std::string hex_str(const Bytes16& b) { return hex_str(b.data(), b.size(), false); }

std::vector<uint8_t> parse_hex(const std::string& s) {
    if (s.size() % 2) throw std::invalid_argument("synth: odd hex length");
    std::vector<uint8_t> out(s.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            throw std::invalid_argument("synth: bad hex digit");
        };
        out[i] = uint8_t(nib(s[2 * i]) << 4 | nib(s[2 * i + 1]));
    }
    return out;
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f) throw std::runtime_error("synth: cannot write " + path);
}

// ---- derived keys -----------------------------------------------------------------------------------------------------------

struct Derived {
    std::array<uint8_t, 32> header_key{};   // always from master key 00
    Bytes16 kak_app{};                      // application key area key for the master key of the NCA
    Bytes16 titlekek{};                     // titlekek for the master key of the NCA
};

Derived derive(const SynthKeys& k, const Bytes16& master) {
    Derived d;
    // header key: kek = D(mk00, aes_kek_gen_src); header_kek = D(kek, header_kek_src); hkk = D(header_kek, aes_key_gen_src);
    // header_key = D(hkk, header_key_src) (two blocks).
    Bytes16 kek = k.aes_kek_generation_source;
    ecb_decrypt(k.master_key_00.data(), kek.data(), 16);
    Bytes16 header_kek = k.header_kek_source;
    ecb_decrypt(kek.data(), header_kek.data(), 16);
    Bytes16 hkk = k.aes_key_generation_source;
    ecb_decrypt(header_kek.data(), hkk.data(), 16);
    d.header_key = k.header_key_source;
    ecb_decrypt(hkk.data(), d.header_key.data(), 32);

    // key area key (application): kek = D(mk, aes_kek_gen_src); src_kek = D(kek, kak_app_src); kak = D(src_kek, aes_key_gen_src)
    Bytes16 kek2 = k.aes_kek_generation_source;
    ecb_decrypt(master.data(), kek2.data(), 16);
    Bytes16 src_kek = k.key_area_key_application_source;
    ecb_decrypt(kek2.data(), src_kek.data(), 16);
    d.kak_app = k.aes_key_generation_source;
    ecb_decrypt(src_kek.data(), d.kak_app.data(), 16);

    // titlekek = D(mk, titlekek_src)
    d.titlekek = k.titlekek_source;
    ecb_decrypt(master.data(), d.titlekek.data(), 16);
    return d;
}

const Bytes16& master_for_generation(const SynthKeys& k, uint8_t key_generation) {
    if (key_generation == 1) return k.master_key_00;
    if (key_generation == 11) return k.master_key_0a;
    throw std::invalid_argument("synth: key_generation must be 1 or 11");
}

// ---- sections ---------------------------------------------------------------------------------------------------------------

bool has_corruption(const std::vector<SynthCorruption>& cs, size_t index, SynthCorruption::Where w) {
    for (const SynthCorruption& c : cs)
        if (c.section == int(index) && c.where == w) return true;
    return false;
}

// Builds one section in plain form: the section header's hash data (+8 .. +0x100) and the section bytes (hash layers + data).
void build_section(const SynthSection& s, size_t index, const std::vector<SynthCorruption>& corrupt, uint8_t* sh,
                   std::vector<uint8_t>& pl) {
    using W = SynthCorruption::Where;
    uint8_t* hd = sh + 8;
    if (s.fs_type == 1) {
        // Hierarchical SHA-256: layer 0 = SHA-256 per 0x1000 block of the PFS0 image; layer 1 = the PFS0 image itself.
        const std::vector<uint8_t>& P = s.data;
        if (P.empty()) throw std::invalid_argument("synth: an empty PFS0 section");
        const size_t nb = (P.size() + 0xFFF) / 0x1000;
        std::vector<uint8_t> table(32 * nb);
        for (size_t k = 0; k < nb; ++k) {
            const size_t n = P.size() - k * 0x1000 < 0x1000 ? P.size() - k * 0x1000 : 0x1000;
            const auto h = synth_sha256(P.data() + k * 0x1000, n);
            std::memcpy(table.data() + 32 * k, h.data(), 32);
        }
        const auto master = synth_sha256(table.data(), table.size());
        const uint64_t data_off = roundup(table.size(), 0x200);
        pl.assign(size_t(roundup(data_off + P.size(), 0x200)), 0);
        std::memcpy(pl.data(), table.data(), table.size());
        std::memcpy(pl.data() + data_off, P.data(), P.size());

        std::memcpy(hd, master.data(), 32);
        put32(hd + 0x20, 0x1000);                 // block size
        put32(hd + 0x24, 2);                      // layer count
        put64(hd + 0x28, 0);                      // layer 0 (hash table) offset
        put64(hd + 0x30, table.size());           // layer 0 size
        put64(hd + 0x38, data_off);               // layer 1 (PFS0) offset
        put64(hd + 0x40, P.size());               // layer 1 size

        if (has_corruption(corrupt, index, W::HashLayer)) pl[0] ^= 0x01;
        if (has_corruption(corrupt, index, W::DataFirstByte)) pl[data_off] ^= 0x01;
        if (has_corruption(corrupt, index, W::DataLastByte)) pl[data_off + P.size() - 1] ^= 0x01;
        if (has_corruption(corrupt, index, W::MasterHash)) hd[0] ^= 0x01;
        return;
    }
    if (s.fs_type != 0) throw std::invalid_argument("synth: fs_type must be 0 (RomFS) or 1 (PFS0)");

    // IVFC (RomFS): six table entries. Entry 5 is the RomFS image; entry k (k < 5) holds the SHA-256 of every 0x4000 block of
    // entry k+1. Entry 0 is hashed as one piece into the master hash. MaxLayers = 7 (= table entries + 1, as src/nca.cpp reads it).
    const std::vector<uint8_t>& R = s.data;
    if (R.empty()) throw std::invalid_argument("synth: an empty RomFS section");
    const uint64_t B = 0x4000;
    std::vector<uint8_t> lev[6];
    lev[5] = R;
    for (int k = 4; k >= 0; --k) {
        const std::vector<uint8_t>& up = lev[k + 1];
        const size_t n = (up.size() + B - 1) / B;
        lev[k].assign(32 * n, 0);
        for (size_t j = 0; j < n; ++j) {
            const size_t len = up.size() - j * B < B ? up.size() - j * B : B;
            const auto h = synth_sha256(up.data() + j * B, len);
            std::memcpy(lev[k].data() + 32 * j, h.data(), 32);
        }
    }
    const auto master = synth_sha256(lev[0].data(), lev[0].size());
    uint64_t off[6];
    off[0] = 0;
    for (int k = 1; k < 6; ++k) off[k] = roundup(off[k - 1] + lev[k - 1].size(), B);
    const uint64_t total = roundup(off[5] + R.size(), 0x200);
    pl.assign(size_t(total), 0);
    for (int k = 0; k < 6; ++k) std::memcpy(pl.data() + off[k], lev[k].data(), lev[k].size());

    std::memcpy(hd, "IVFC", 4);
    put32(hd + 4, 0x10000);                       // magic number
    put32(hd + 8, 0x20);                          // master hash size
    put32(hd + 0xC, 7);                           // MaxLayers
    for (int k = 0; k < 6; ++k) {
        uint8_t* e = hd + 0x10 + 0x18 * k;
        put64(e, off[k]);
        put64(e + 8, lev[k].size());
        put32(e + 16, 14);                        // block size 2^14
        put32(e + 20, 0);
    }
    std::memcpy(hd + 0xC0, master.data(), 32);

    if (has_corruption(corrupt, index, W::HashLayer)) pl[off[4]] ^= 0x01;
    if (has_corruption(corrupt, index, W::DataFirstByte)) pl[off[5]] ^= 0x01;
    if (has_corruption(corrupt, index, W::DataLastByte)) pl[off[5] + R.size() - 1] ^= 0x01;
    if (has_corruption(corrupt, index, W::MasterHash)) hd[0xC0] ^= 0x01;
}

// ---- NSO / ExeFS / cnmt -----------------------------------------------------------------------------------------------------

std::vector<uint8_t> nso_file(const std::vector<uint8_t>& build20, uint32_t seed) {
    std::vector<uint8_t> n(0x200);
    fill_seeded(n.data(), n.size(), seed);
    std::memcpy(n.data(), "NSO0", 4);
    put32(n.data() + 0x0C, 0);                    // flags
    put32(n.data() + 0x10 + 8, 0x80);             // text decompressed size
    put32(n.data() + 0x20 + 8, 0x80);             // ro decompressed size
    put32(n.data() + 0x30 + 8, 0x80);             // data decompressed size
    std::memset(n.data() + 0x40, 0, 0x20);
    std::memcpy(n.data() + 0x40, build20.data(), 20);   // module id = build id (first 20 bytes)
    put32(n.data() + 0x60, 0x20);                 // compressed sizes
    put32(n.data() + 0x64, 0x20);
    put32(n.data() + 0x68, 0x20);
    return n;
}

// 16 lower-case hex digits of a title id, most significant first.
std::string title_hex(uint64_t t) {
    uint8_t b[8];
    put64(b, t);
    std::string s;
    for (int i = 7; i >= 0; --i) s += hex_str(b + i, 1, false);
    return s;
}

std::vector<uint8_t> cnmt_pfs(uint64_t title_id, uint32_t version) {
    std::vector<uint8_t> c(0x20, 0);
    put64(c.data(), title_id);
    put32(c.data() + 8, version);
    c[0xC] = 0x80;
    return synth_pfs0({{"Application_" + title_hex(title_id) + ".cnmt", c}});
}

}  // namespace

// ---- public API -------------------------------------------------------------------------------------------------------------

SynthKeys SynthKeys::standard() {
    SynthKeys k;
    k.master_key_00 = seeded16(1);
    k.master_key_0a = seeded16(2);
    k.aes_kek_generation_source = seeded16(3);
    k.aes_key_generation_source = seeded16(4);
    k.titlekek_source = seeded16(5);
    k.key_area_key_application_source = seeded16(6);
    k.header_kek_source = seeded16(7);
    fill_seeded(k.header_key_source.data(), 32, 8);
    return k;
}

std::string write_synth_keys_file_with(const std::string& dir, const SynthKeys& k, const std::string& file_name,
                                       const std::vector<std::pair<Bytes16, Bytes16>>& extra_title_keys) {
    std::filesystem::create_directories(dir);
    const std::string path = dir + "/" + file_name;
    std::string text;
    text += "# synthetic test keys (not real Switch keys)\n";
    text += "master_key_00 = " + hex_str(k.master_key_00) + "\n";
    text += "master_key_0a = " + hex_str(k.master_key_0a) + "\n";
    text += "aes_kek_generation_source = " + hex_str(k.aes_kek_generation_source) + "\n";
    text += "aes_key_generation_source = " + hex_str(k.aes_key_generation_source) + "\n";
    text += "titlekek_source = " + hex_str(k.titlekek_source) + "\n";
    text += "key_area_key_application_source = " + hex_str(k.key_area_key_application_source) + "\n";
    text += "header_kek_source = " + hex_str(k.header_kek_source) + "\n";
    text += "header_key_source = " + hex_str(k.header_key_source.data(), 32, false) + "\n";
    for (const auto& tk : extra_title_keys)
        text += hex_str(tk.first) + " = " + hex_str(tk.second) + "\n";
    std::ofstream f(path, std::ios::binary);
    f << text;
    if (!f) throw std::runtime_error("synth: cannot write " + path);
    return path;
}

std::string write_synth_keys_file(const std::string& dir) {
    return write_synth_keys_file_with(dir, SynthKeys::standard(), "prod.keys");
}

Bytes16 synth_encrypt_titlekey(uint8_t key_generation, const Bytes16& plain_titlekey) {
    const SynthKeys k = SynthKeys::standard();
    const Derived d = derive(k, master_for_generation(k, key_generation));
    Bytes16 out = plain_titlekey;
    ecb_encrypt(d.titlekek.data(), out.data(), 16);
    return out;
}

void synth_aes128_encrypt_block(const uint8_t key[16], uint8_t block[16]) { Aes128(key).encrypt(block); }

SynthNcaBuild synth_nca_build(const SynthNcaSpec& sp) {
    if (sp.sections.size() > 4) throw std::invalid_argument("synth_nca: at most 4 sections");
    if (sp.magic_digit != '3' && sp.magic_digit != '2' && sp.magic_digit != '0')
        throw std::invalid_argument("synth_nca: magic_digit must be 3, 2 or 0");
    if (sp.key_area_kind != 0) throw std::invalid_argument("synth_nca: only key area kind 0 is derived");
    const SynthKeys keys = SynthKeys::standard();
    const Derived d = derive(keys, master_for_generation(keys, sp.key_generation));
    const Derived d00 = derive(keys, keys.master_key_00);   // header key (always master key 00)
    const uint8_t* hk = d00.header_key.data();

    // Key area (plain 0x40 bytes; entry 2 at 0x20 is the AES-CTR key) and its encrypted form stored at 0x300.
    uint8_t area[0x40];
    fill_seeded(area, sizeof area, 100u + sp.key_generation);
    uint8_t enc_area[0x40];
    std::memcpy(enc_area, area, sizeof area);
    ecb_encrypt(d.kak_app.data(), enc_area, sizeof enc_area);
    Bytes16 content_key{};
    if (sp.has_rights) std::memcpy(content_key.data(), sp.titlekey.data(), 16);
    else std::memcpy(content_key.data(), area + 0x20, 16);

    std::vector<uint8_t> hdr(0xC00, 0);
    std::vector<uint8_t> body;
    SynthNcaBuild out;
    out.plain.assign(4, {});
    uint64_t pos = 0xC00;
    for (size_t i = 0; i < sp.sections.size(); ++i) {
        const SynthSection& s = sp.sections[i];
        uint8_t sh[0x200] = {};
        std::vector<uint8_t> pl;
        build_section(s, i, sp.corrupt, sh, pl);
        sh[0] = 2;
        sh[2] = s.fs_type;
        sh[3] = uint8_t(s.fs_type == 1 ? 2 : 3);
        sh[4] = s.enc_type;
        out.plain[i] = pl;
        std::vector<uint8_t> enc = pl;
        if (s.enc_type == 3) {
            uint8_t hi[8];
            fill_seeded(hi, 8, 0x200u + uint32_t(i));
            for (int j = 0; j < 8; ++j) sh[0x140 + j] = hi[7 - j];   // stored reversed (reader: ctr_hi[k] = header[0x147 - k])
            uint8_t ctr[16];
            std::memcpy(ctr, hi, 8);
            for (int j = 0; j < 8; ++j) ctr[8 + j] = uint8_t((pos / 16) >> (56 - 8 * j));   // big-endian block number
            ctr_crypt(content_key.data(), ctr, enc.data(), enc.size());
        } else if (s.enc_type != 1) {
            throw std::invalid_argument("synth_nca: enc_type must be 1 or 3");
        }
        put32(hdr.data() + 0x240 + 0x10 * i, uint32_t(pos / 0x200));
        put32(hdr.data() + 0x244 + 0x10 * i, uint32_t((pos + enc.size()) / 0x200));
        std::memcpy(hdr.data() + 0x400 + 0x200 * i, sh, 0x200);
        body.insert(body.end(), enc.begin(), enc.end());
        pos += enc.size();
    }

    const uint64_t total = 0xC00 + body.size();
    std::memcpy(hdr.data() + 0x200, "NCA", 3);
    hdr[0x203] = uint8_t(sp.magic_digit);
    hdr[0x205] = sp.content_type;
    hdr[0x206] = 0;                                   // crypto type 1 (unused; 0x220 holds the generation)
    hdr[0x207] = sp.key_area_kind;
    put64(hdr.data() + 0x208, total);
    put64(hdr.data() + 0x210, sp.program_id);
    put32(hdr.data() + 0x218, sp.content_index);
    hdr[0x220] = sp.key_generation;
    if (sp.has_rights) std::memcpy(hdr.data() + 0x230, sp.rights_id.data(), 16);
    std::memcpy(hdr.data() + 0x300, enc_area, sizeof enc_area);

    // Header: XTS with the header key. NCA3/NCA0: sectors 0..5 in one run. NCA2: sectors 0..1, then each section header alone at sector 0.
    if (sp.magic_digit == '2') {
        xts(hk, hk + 16, 0, 0x200, hdr.data(), 0x400, true);
        for (int i = 0; i < 4; ++i) xts(hk, hk + 16, 0, 0x200, hdr.data() + 0x400 + 0x200 * i, 0x200, true);
    } else {
        xts(hk, hk + 16, 0, 0x200, hdr.data(), 0xC00, true);
    }

    out.bytes = hdr;
    out.bytes.insert(out.bytes.end(), body.begin(), body.end());
    if (sp.truncate && sp.truncate_to < out.bytes.size()) out.bytes.resize(sp.truncate_to);
    return out;
}

std::vector<uint8_t> synth_nca(const SynthNcaSpec& spec) { return synth_nca_build(spec).bytes; }

std::vector<uint8_t> synth_ticket(const Bytes16& rights_id, const Bytes16& titlekey_encrypted, bool personalized) {
    std::vector<uint8_t> t(0x2C0, 0);
    put32(t.data(), 0x10004);                                  // RSA-2048 PKCS#1 v1.5, SHA-256
    fill_seeded(t.data() + 4, 0x100, 300);                       // signature (random; nothing checks it)
    const char issuer[] = "Root-CA00000003-XS00000020";
    std::memcpy(t.data() + 0x140, issuer, sizeof issuer - 1);   // issuer
    std::memcpy(t.data() + 0x180, titlekey_encrypted.data(), 16);   // title key block, key first
    t[0x280] = 2;                                                // format version
    t[0x281] = personalized ? 1 : 0;                             // title key type: 0 common, 1 personalized
    std::memcpy(t.data() + 0x2A0, rights_id.data(), 16);        // rights id, body offset 0x160
    return t;
}

std::vector<uint8_t> synth_exefs(const std::string& build_id_40_hex) {
    const std::vector<uint8_t> build = parse_hex(build_id_40_hex);
    if (build.size() != 20) throw std::invalid_argument("synth_exefs: build id must be 40 hex digits");
    std::vector<uint8_t> npdm(0x80);
    fill_seeded(npdm.data(), npdm.size(), 5);
    return synth_pfs0({{"main", nso_file(build, 11)},
                       {"rtld", nso_file(build, 12)},
                       {"sdk", nso_file(build, 13)},
                       {"subsdk0", nso_file(build, 14)},
                       {"main.npdm", npdm}});
}

std::vector<uint8_t> synth_cnmt_nca(uint64_t title_id, uint32_t version) {
    SynthNcaSpec sp;
    sp.content_type = 1;
    sp.program_id = title_id;
    sp.key_generation = 11;
    sp.sections = {SynthSection{1, 3, cnmt_pfs(title_id, version)}};
    return synth_nca(sp);
}

std::vector<uint8_t> synth_nsp(const SynthFiles& entries) { return synth_pfs0(entries); }

std::vector<uint8_t> synth_xci(const SynthFiles& secure_entries, bool include_secure) {
    SynthFiles root;
    root.push_back({"update", synth_hfs0({})});
    if (include_secure) root.push_back({"secure", synth_hfs0(secure_entries)});
    const std::vector<uint8_t> rootimg = synth_hfs0(root);
    std::vector<uint8_t> out(0x200, 0);
    std::memcpy(out.data() + 0x100, "HEAD", 4);
    put64(out.data() + 0x138, 0x200);
    out.insert(out.end(), rootimg.begin(), rootimg.end());
    return out;
}

Bytes16 synth_dump_rights_id() {
    Bytes16 b = seeded16(77);
    b[0] |= 0x01;   // never all zero
    return b;
}
Bytes16 synth_dump_titlekey() { return seeded16(78); }

SynthDump make_synth_dump(const char* kind, uint64_t program_id, const std::string& build_id_40_hex, const SynthFiles& romfs_files,
                          bool use_ticket, const std::string& dir) {
    const std::string k = kind;
    if (k != "nsp" && k != "xci") throw std::invalid_argument("make_synth_dump: kind must be nsp or xci");
    std::vector<uint8_t> build = parse_hex(build_id_40_hex);
    if (build.size() != 20) throw std::invalid_argument("make_synth_dump: build id must be 40 hex digits");

    std::string folder = dir;
    if (folder.empty()) {
        folder = (std::filesystem::temp_directory_path() / ("mprs_synth_" + k + "_" + title_hex(program_id) +
                                                              (use_ticket ? "_tik" : ""))).string();
    }
    std::filesystem::create_directories(folder);

    SynthNcaSpec prog;
    prog.content_type = 0;
    prog.program_id = program_id;
    prog.content_index = 0;
    prog.key_generation = 11;
    if (use_ticket) {
        prog.has_rights = true;
        prog.rights_id = synth_dump_rights_id();
        prog.titlekey = synth_dump_titlekey();
    }
    prog.sections = {SynthSection{1, 3, synth_exefs(build_id_40_hex)}, SynthSection{0, 3, synth_romfs(romfs_files)}};
    const std::vector<uint8_t> prog_nca = synth_nca(prog);
    const std::vector<uint8_t> meta_nca = synth_cnmt_nca(program_id, 0);

    SynthFiles entries = {{title_hex(program_id) + ".nca", prog_nca}, {title_hex(program_id) + ".cnmt.nca", meta_nca}};
    if (use_ticket) {
        const Bytes16 rights = synth_dump_rights_id();
        entries.push_back({hex_str(rights) + ".tik",
                           synth_ticket(rights, synth_encrypt_titlekey(11, synth_dump_titlekey()), false)});
    }
    const std::vector<uint8_t> image = k == "nsp" ? synth_nsp(entries) : synth_xci(entries);

    SynthDump d;
    d.path = folder + "/game." + k;
    write_bytes(d.path, image);
    d.keys_file_path = write_synth_keys_file(folder);
    d.build_id = hex_str(build.data(), build.size(), true);
    d.program_id = program_id;
    d.romfs_files = romfs_files;
    return d;
}

}  // namespace mprs_test
