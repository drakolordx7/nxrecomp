// SPDX-License-Identifier: GPL-3.0-or-later
// AES-128 (ECB/CTR/XTS) and SHA-1/SHA-256, written for this project from FIPS 197 and FIPS 180-4 (no third-party code, no system crypto
// library). AES runs on the AES-NI instructions when the CPU has them, else on lookup tables built at first use; the hashes are portable code.
// Every table is computed from its definition, not typed in. No key bytes are ever logged.
#include "crypto.h"

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#define NXS_X86 1
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace nxs {
namespace {

void xor_into(u8* dst, const u8* src, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 a, b;
        std::memcpy(&a, dst + i, 8);
        std::memcpy(&b, src + i, 8);
        a ^= b;
        std::memcpy(dst + i, &a, 8);
    }
    for (; i < n; ++i) dst[i] ^= src[i];
}

u64 load_be64(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
void store_be64(u8* p, u64 v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<u8>(v);
        v >>= 8;
    }
}
u32 load_be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]); }
void store_be32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}
u32 rotr32(u32 v, int n) { return (v >> n) | (v << (32 - n)); }
u32 rotl32(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

// Multiply the XTS tweak by x in GF(2^128), little-endian convention: byte 0 is the least significant.
void gf128_mul_x(u8 t[16]) {
    u8 carry = 0;
    for (int i = 0; i < 16; ++i) {
        const u8 next = static_cast<u8>(t[i] >> 7);
        t[i] = static_cast<u8>((t[i] << 1) | carry);
        carry = next;
    }
    if (carry) t[0] ^= 0x87;
}

// ---- AES tables (software path) --------------------------------------------------------------------------------------

struct AesTables {
    u8 sbox[256], inv[256];
    u32 te0[256], td0[256];
    AesTables() {
        // The S-box: multiplicative inverse in GF(2^8) followed by the affine map (the usual generation by walking p = 3^k with q = 1/p).
        u8 p = 1, q = 1;
        do {
            p = static_cast<u8>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q = static_cast<u8>(q ^ (q << 1));
            q = static_cast<u8>(q ^ (q << 2));
            q = static_cast<u8>(q ^ (q << 4));
            if (q & 0x80) q ^= 0x09;
            const u8 x = static_cast<u8>(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
            sbox[p] = static_cast<u8>(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) inv[sbox[i]] = static_cast<u8>(i);
        for (int i = 0; i < 256; ++i) {
            const u8 s = sbox[i];
            te0[i] = (u32(mul(s, 2)) << 24) | (u32(s) << 16) | (u32(s) << 8) | u32(mul(s, 3));
            const u8 v = inv[i];
            td0[i] = (u32(mul(v, 14)) << 24) | (u32(mul(v, 9)) << 16) | (u32(mul(v, 13)) << 8) | u32(mul(v, 11));
        }
    }
    static u8 mul(u8 a, u8 b) {
        u8 r = 0;
        while (b) {
            if (b & 1) r ^= a;
            a = static_cast<u8>((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
            b >>= 1;
        }
        return r;
    }
};
const AesTables& tables() {
    static const AesTables t;
    return t;
}

void soft_expand(const u8 key[16], u32 enc[44], u32 dec[44]) {
    const AesTables& T = tables();
    for (int i = 0; i < 4; ++i) enc[i] = load_be32(key + 4 * i);
    u8 rcon = 1;
    for (int i = 4; i < 44; ++i) {
        u32 t = enc[i - 1];
        if (i % 4 == 0) {
            t = rotl32(t, 8);
            t = (u32(T.sbox[t >> 24]) << 24) | (u32(T.sbox[(t >> 16) & 0xFF]) << 16) | (u32(T.sbox[(t >> 8) & 0xFF]) << 8) | u32(T.sbox[t & 0xFF]);
            t ^= u32(rcon) << 24;
            rcon = static_cast<u8>((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
        }
        enc[i] = enc[i - 4] ^ t;
    }
    // Decryption keys: the round keys in reverse order, InvMixColumns applied to the inner rounds.
    for (int r = 0; r <= 10; ++r) {
        for (int j = 0; j < 4; ++j) {
            u32 w = enc[4 * (10 - r) + j];
            if (r > 0 && r < 10) {
                w = T.td0[T.sbox[w >> 24]] ^ rotr32(T.td0[T.sbox[(w >> 16) & 0xFF]], 8) ^ rotr32(T.td0[T.sbox[(w >> 8) & 0xFF]], 16) ^
                    rotr32(T.td0[T.sbox[w & 0xFF]], 24);
            }
            dec[4 * r + j] = w;
        }
    }
}

void soft_encrypt_block(const u32 rk[44], const u8* in, u8* out) {
    const AesTables& T = tables();
    u32 s0 = load_be32(in) ^ rk[0], s1 = load_be32(in + 4) ^ rk[1], s2 = load_be32(in + 8) ^ rk[2], s3 = load_be32(in + 12) ^ rk[3];
    for (int r = 1; r < 10; ++r) {
        const u32 t0 = T.te0[s0 >> 24] ^ rotr32(T.te0[(s1 >> 16) & 0xFF], 8) ^ rotr32(T.te0[(s2 >> 8) & 0xFF], 16) ^ rotr32(T.te0[s3 & 0xFF], 24) ^ rk[4 * r];
        const u32 t1 = T.te0[s1 >> 24] ^ rotr32(T.te0[(s2 >> 16) & 0xFF], 8) ^ rotr32(T.te0[(s3 >> 8) & 0xFF], 16) ^ rotr32(T.te0[s0 & 0xFF], 24) ^ rk[4 * r + 1];
        const u32 t2 = T.te0[s2 >> 24] ^ rotr32(T.te0[(s3 >> 16) & 0xFF], 8) ^ rotr32(T.te0[(s0 >> 8) & 0xFF], 16) ^ rotr32(T.te0[s1 & 0xFF], 24) ^ rk[4 * r + 2];
        const u32 t3 = T.te0[s3 >> 24] ^ rotr32(T.te0[(s0 >> 16) & 0xFF], 8) ^ rotr32(T.te0[(s1 >> 8) & 0xFF], 16) ^ rotr32(T.te0[s2 & 0xFF], 24) ^ rk[4 * r + 3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    const u8* S = T.sbox;
    store_be32(out, ((u32(S[s0 >> 24]) << 24) | (u32(S[(s1 >> 16) & 0xFF]) << 16) | (u32(S[(s2 >> 8) & 0xFF]) << 8) | u32(S[s3 & 0xFF])) ^ rk[40]);
    store_be32(out + 4, ((u32(S[s1 >> 24]) << 24) | (u32(S[(s2 >> 16) & 0xFF]) << 16) | (u32(S[(s3 >> 8) & 0xFF]) << 8) | u32(S[s0 & 0xFF])) ^ rk[41]);
    store_be32(out + 8, ((u32(S[s2 >> 24]) << 24) | (u32(S[(s3 >> 16) & 0xFF]) << 16) | (u32(S[(s0 >> 8) & 0xFF]) << 8) | u32(S[s1 & 0xFF])) ^ rk[42]);
    store_be32(out + 12, ((u32(S[s3 >> 24]) << 24) | (u32(S[(s0 >> 16) & 0xFF]) << 16) | (u32(S[(s1 >> 8) & 0xFF]) << 8) | u32(S[s2 & 0xFF])) ^ rk[43]);
}

void soft_decrypt_block(const u32 rk[44], const u8* in, u8* out) {
    const AesTables& T = tables();
    u32 s0 = load_be32(in) ^ rk[0], s1 = load_be32(in + 4) ^ rk[1], s2 = load_be32(in + 8) ^ rk[2], s3 = load_be32(in + 12) ^ rk[3];
    for (int r = 1; r < 10; ++r) {
        const u32 t0 = T.td0[s0 >> 24] ^ rotr32(T.td0[(s3 >> 16) & 0xFF], 8) ^ rotr32(T.td0[(s2 >> 8) & 0xFF], 16) ^ rotr32(T.td0[s1 & 0xFF], 24) ^ rk[4 * r];
        const u32 t1 = T.td0[s1 >> 24] ^ rotr32(T.td0[(s0 >> 16) & 0xFF], 8) ^ rotr32(T.td0[(s3 >> 8) & 0xFF], 16) ^ rotr32(T.td0[s2 & 0xFF], 24) ^ rk[4 * r + 1];
        const u32 t2 = T.td0[s2 >> 24] ^ rotr32(T.td0[(s1 >> 16) & 0xFF], 8) ^ rotr32(T.td0[(s0 >> 8) & 0xFF], 16) ^ rotr32(T.td0[s3 & 0xFF], 24) ^ rk[4 * r + 2];
        const u32 t3 = T.td0[s3 >> 24] ^ rotr32(T.td0[(s2 >> 16) & 0xFF], 8) ^ rotr32(T.td0[(s1 >> 8) & 0xFF], 16) ^ rotr32(T.td0[s0 & 0xFF], 24) ^ rk[4 * r + 3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    const u8* S = T.inv;
    store_be32(out, ((u32(S[s0 >> 24]) << 24) | (u32(S[(s3 >> 16) & 0xFF]) << 16) | (u32(S[(s2 >> 8) & 0xFF]) << 8) | u32(S[s1 & 0xFF])) ^ rk[40]);
    store_be32(out + 4, ((u32(S[s1 >> 24]) << 24) | (u32(S[(s0 >> 16) & 0xFF]) << 16) | (u32(S[(s3 >> 8) & 0xFF]) << 8) | u32(S[s2 & 0xFF])) ^ rk[41]);
    store_be32(out + 8, ((u32(S[s2 >> 24]) << 24) | (u32(S[(s1 >> 16) & 0xFF]) << 16) | (u32(S[(s0 >> 8) & 0xFF]) << 8) | u32(S[s3 & 0xFF])) ^ rk[42]);
    store_be32(out + 12, ((u32(S[s3 >> 24]) << 24) | (u32(S[(s2 >> 16) & 0xFF]) << 16) | (u32(S[(s1 >> 8) & 0xFF]) << 8) | u32(S[s0 & 0xFF])) ^ rk[43]);
}

#ifdef NXS_X86
// ---- AES-NI ----------------------------------------------------------------------------------------------------------

bool cpu_has_aesni() {
    static const bool has = [] {
        unsigned a = 0, b = 0, c = 0, d = 0;
        return __get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 25)) != 0 && (c & (1u << 19)) != 0;   // AES and SSE4.1
    }();
    return has;
}

__attribute__((target("aes,sse4.1"))) inline __m128i key_step(__m128i k, __m128i gen) {
    gen = _mm_shuffle_epi32(gen, 0xFF);
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    k = _mm_xor_si128(k, _mm_slli_si128(k, 4));
    return _mm_xor_si128(k, gen);
}

__attribute__((target("aes,sse4.1"))) void ni_expand(const u8 key[16], u8 enc[176], u8 dec[176]) {
    __m128i k[11];
    k[0] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key));
#define NXS_RK(i, rc) k[i] = key_step(k[i - 1], _mm_aeskeygenassist_si128(k[i - 1], rc))
    NXS_RK(1, 0x01); NXS_RK(2, 0x02); NXS_RK(3, 0x04); NXS_RK(4, 0x08); NXS_RK(5, 0x10);
    NXS_RK(6, 0x20); NXS_RK(7, 0x40); NXS_RK(8, 0x80); NXS_RK(9, 0x1B); NXS_RK(10, 0x36);
#undef NXS_RK
    for (int i = 0; i <= 10; ++i) _mm_storeu_si128(reinterpret_cast<__m128i*>(enc + 16 * i), k[i]);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dec), k[10]);
    for (int i = 1; i < 10; ++i) _mm_storeu_si128(reinterpret_cast<__m128i*>(dec + 16 * i), _mm_aesimc_si128(k[10 - i]));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dec + 160), k[0]);
}

__attribute__((target("aes,sse4.1"))) void ni_ecb(const u8* rk, const u8* in, u8* out, size_t blocks, bool encrypt) {
    __m128i k[11];
    for (int i = 0; i < 11; ++i) k[i] = _mm_load_si128(reinterpret_cast<const __m128i*>(rk + 16 * i));
    size_t b = 0;
    for (; b + 4 <= blocks; b += 4) {
        __m128i s0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 16 * b));
        __m128i s1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 16 * b + 16));
        __m128i s2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 16 * b + 32));
        __m128i s3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 16 * b + 48));
        s0 = _mm_xor_si128(s0, k[0]), s1 = _mm_xor_si128(s1, k[0]), s2 = _mm_xor_si128(s2, k[0]), s3 = _mm_xor_si128(s3, k[0]);
        if (encrypt) {
            for (int r = 1; r < 10; ++r) {
                s0 = _mm_aesenc_si128(s0, k[r]), s1 = _mm_aesenc_si128(s1, k[r]), s2 = _mm_aesenc_si128(s2, k[r]), s3 = _mm_aesenc_si128(s3, k[r]);
            }
            s0 = _mm_aesenclast_si128(s0, k[10]), s1 = _mm_aesenclast_si128(s1, k[10]), s2 = _mm_aesenclast_si128(s2, k[10]), s3 = _mm_aesenclast_si128(s3, k[10]);
        } else {
            for (int r = 1; r < 10; ++r) {
                s0 = _mm_aesdec_si128(s0, k[r]), s1 = _mm_aesdec_si128(s1, k[r]), s2 = _mm_aesdec_si128(s2, k[r]), s3 = _mm_aesdec_si128(s3, k[r]);
            }
            s0 = _mm_aesdeclast_si128(s0, k[10]), s1 = _mm_aesdeclast_si128(s1, k[10]), s2 = _mm_aesdeclast_si128(s2, k[10]), s3 = _mm_aesdeclast_si128(s3, k[10]);
        }
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16 * b), s0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16 * b + 16), s1);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16 * b + 32), s2);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16 * b + 48), s3);
    }
    for (; b < blocks; ++b) {
        __m128i s = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 16 * b)), k[0]);
        for (int r = 1; r < 10; ++r) s = encrypt ? _mm_aesenc_si128(s, k[r]) : _mm_aesdec_si128(s, k[r]);
        s = encrypt ? _mm_aesenclast_si128(s, k[10]) : _mm_aesdeclast_si128(s, k[10]);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16 * b), s);
    }
}
#endif

}  // namespace

// ---- AES-128-ECB -----------------------------------------------------------------------------------------------------

Aes128Ecb::Aes128Ecb(const u8 key[16]) {
#ifdef NXS_X86
    ni_ = cpu_has_aesni();
    if (ni_) ni_expand(key, enc_, dec_);
#endif
    soft_expand(key, soft_enc_, soft_dec_);
}

void Aes128Ecb::encrypt(const void* in, void* out, size_t len) const {
    if (len % 16 != 0) fail(Fail::Generic, "AES: %zu bytes is not a multiple of 16", len);
    const u8* src = static_cast<const u8*>(in);
    u8* dst = static_cast<u8*>(out);
#ifdef NXS_X86
    if (ni_) {
        ni_ecb(enc_, src, dst, len / 16, true);
        return;
    }
#endif
    for (size_t i = 0; i < len; i += 16) soft_encrypt_block(soft_enc_, src + i, dst + i);   // in == out is fine: a block is read whole first
}

void Aes128Ecb::decrypt(const void* in, void* out, size_t len) const {
    if (len % 16 != 0) fail(Fail::Generic, "AES: %zu bytes is not a multiple of 16", len);
    const u8* src = static_cast<const u8*>(in);
    u8* dst = static_cast<u8*>(out);
#ifdef NXS_X86
    if (ni_) {
        ni_ecb(dec_, src, dst, len / 16, false);
        return;
    }
#endif
    for (size_t i = 0; i < len; i += 16) soft_decrypt_block(soft_dec_, src + i, dst + i);
}

// ---- AES-128-CTR -----------------------------------------------------------------------------------------------------

void aes_ctr_crypt(const u8 key[16], const u8 counter[16], void* data, size_t len) {
    if (len == 0) return;
    Aes128Ecb aes(key);
    constexpr size_t kChunk = 64 * 1024;
    std::vector<u8> ks(kChunk);
    u64 hi = load_be64(counter);
    u64 lo = load_be64(counter + 8);
    u8* p = static_cast<u8*>(data);
    while (len > 0) {
        const size_t n = std::min(len, kChunk);
        const size_t blocks = (n + 15) / 16;
        for (size_t b = 0; b < blocks; ++b) {
            store_be64(&ks[b * 16], hi);
            store_be64(&ks[b * 16 + 8], lo);
            if (++lo == 0) ++hi;
        }
        aes.encrypt(ks.data(), ks.data(), blocks * 16);
        xor_into(p, ks.data(), n);
        p += n;
        len -= n;
    }
}

// ---- AES-128-XTS, Nintendo variant ----------------------------------------------------------------------------------

namespace {

void xts_nintendo(const u8 key1[16], const u8 key2[16], u64 first_sector, u32 sector_size, void* data, size_t len,
                  bool encrypt) {
    if (sector_size == 0 || sector_size % 16 != 0)
        fail(Fail::Generic, "XTS: sector size %lu is not a multiple of 16", static_cast<unsigned long>(sector_size));
    if (len % sector_size != 0)
        fail(Fail::Generic, "XTS: %zu bytes is not a whole number of %lu-byte sectors", len,
             static_cast<unsigned long>(sector_size));
    Aes128Ecb data_key(key1);
    Aes128Ecb tweak_key(key2);
    std::vector<u8> tw(sector_size);
    u8* p = static_cast<u8*>(data);
    const size_t sectors = len / sector_size;
    for (size_t s = 0; s < sectors; ++s) {
        // Nintendo: sector number big-endian in bytes 8..15, bytes 0..7 zero.
        u8 t[16] = {};
        store_be64(t + 8, first_sector + s);
        tweak_key.encrypt(t, t, 16);   // T_0 = E(key2, tweak input)
        for (size_t j = 0; j < sector_size; j += 16) {
            std::memcpy(&tw[j], t, 16);
            gf128_mul_x(t);
        }
        // C = E(key1, P ^ T) ^ T, applied to the whole sector. ECB over the sector is the same as block by block.
        u8* sec = p + s * sector_size;
        xor_into(sec, tw.data(), sector_size);
        if (encrypt) data_key.encrypt(sec, sec, sector_size);
        else data_key.decrypt(sec, sec, sector_size);
        xor_into(sec, tw.data(), sector_size);
    }
}

}  // namespace

void aes_xts_encrypt_nintendo(const u8 key1[16], const u8 key2[16], u64 first_sector, u32 sector_size, void* data,
                              size_t len) {
    xts_nintendo(key1, key2, first_sector, sector_size, data, len, true);
}

void aes_xts_decrypt_nintendo(const u8 key1[16], const u8 key2[16], u64 first_sector, u32 sector_size, void* data,
                              size_t len) {
    xts_nintendo(key1, key2, first_sector, sector_size, data, len, false);
}

// ---- SHA-1 / SHA-256 -------------------------------------------------------------------------------------------------

namespace {

struct Sha256Consts {
    u32 k[64];
    u32 h0[8];
    Sha256Consts() {
        // K: the first 32 bits of the fractional parts of the cube roots of the first 64 primes; H0: of the square roots of the first 8.
        // Both are integer roots of p * 2^96 and p * 2^64 (128-bit arithmetic), so no floating point is involved.
        int primes[64], n = 0;
        for (int c = 2; n < 64; ++c) {
            bool prime = true;
            for (int d = 2; d * d <= c; ++d)
                if (c % d == 0) prime = false;
            if (prime) primes[n++] = c;
        }
        for (int i = 0; i < 64; ++i) {
            const unsigned __int128 target = static_cast<unsigned __int128>(primes[i]) << 96;
            u64 lo = 0, hi = u64(1) << 35;
            while (lo < hi) {
                const u64 mid = lo + (hi - lo + 1) / 2;
                const unsigned __int128 m = mid;
                if (m * m * m <= target) lo = mid;
                else hi = mid - 1;
            }
            k[i] = static_cast<u32>(lo);
        }
        for (int i = 0; i < 8; ++i) {
            const unsigned __int128 target = static_cast<unsigned __int128>(primes[i]) << 64;
            u64 lo = 0, hi = u64(1) << 36;
            while (lo < hi) {
                const u64 mid = lo + (hi - lo + 1) / 2;
                const unsigned __int128 m = mid;
                if (m * m <= target) lo = mid;
                else hi = mid - 1;
            }
            h0[i] = static_cast<u32>(lo);
        }
    }
};
const Sha256Consts& sha256_consts() {
    static const Sha256Consts c;
    return c;
}

}  // namespace

Sha256::Sha256() {
    std::memcpy(h_, sha256_consts().h0, sizeof h_);
    std::memset(buf_, 0, sizeof buf_);
}

void Sha256::block(const u8* p) {
    const u32* K = sha256_consts().k;
    u32 w[64];
    for (int i = 0; i < 16; ++i) w[i] = load_be32(p + 4 * i);
    for (int i = 16; i < 64; ++i) {
        const u32 s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const u32 s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const u32 S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const u32 ch = (e & f) ^ (~e & g);
        const u32 t1 = h + S1 + ch + K[i] + w[i];
        const u32 S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const u32 maj = (a & b) ^ (a & c) ^ (b & c);
        const u32 t2 = S0 + maj;
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
    if (done_) fail(Fail::Generic, "SHA-256: update after finish");
    const u8* p = static_cast<const u8*>(data);
    total_ += len;
    if (fill_ > 0) {
        const size_t n = std::min(len, sizeof buf_ - fill_);
        std::memcpy(buf_ + fill_, p, n);
        fill_ += n, p += n, len -= n;
        if (fill_ < sizeof buf_) return;
        block(buf_);
        fill_ = 0;
    }
    for (; len >= 64; p += 64, len -= 64) block(p);
    if (len) {
        std::memcpy(buf_, p, len);
        fill_ = len;
    }
}

std::array<u8, 32> Sha256::finish() {
    if (done_) fail(Fail::Generic, "SHA-256: finish called twice");
    done_ = true;
    const u64 bits = total_ * 8;
    buf_[fill_++] = 0x80;
    if (fill_ > 56) {
        std::memset(buf_ + fill_, 0, sizeof buf_ - fill_);
        block(buf_);
        fill_ = 0;
    }
    std::memset(buf_ + fill_, 0, 56 - fill_);
    store_be64(buf_ + 56, bits);
    block(buf_);
    std::array<u8, 32> out{};
    for (int i = 0; i < 8; ++i) store_be32(out.data() + 4 * i, h_[i]);
    return out;
}

Sha1::Sha1() {
    h_[0] = 0x67452301u, h_[1] = 0xEFCDAB89u, h_[2] = 0x98BADCFEu, h_[3] = 0x10325476u, h_[4] = 0xC3D2E1F0u;
    std::memset(buf_, 0, sizeof buf_);
}

void Sha1::block(const u8* p) {
    u32 w[80];
    for (int i = 0; i < 16; ++i) w[i] = load_be32(p + 4 * i);
    for (int i = 16; i < 80; ++i) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        u32 f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
        else f = b ^ c ^ d, k = 0xCA62C1D6u;
        const u32 t = rotl32(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rotl32(b, 30), b = a, a = t;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
}

void Sha1::update(const void* data, size_t len) {
    if (done_) fail(Fail::Generic, "SHA-1: update after finish");
    const u8* p = static_cast<const u8*>(data);
    total_ += len;
    if (fill_ > 0) {
        const size_t n = std::min(len, sizeof buf_ - fill_);
        std::memcpy(buf_ + fill_, p, n);
        fill_ += n, p += n, len -= n;
        if (fill_ < sizeof buf_) return;
        block(buf_);
        fill_ = 0;
    }
    for (; len >= 64; p += 64, len -= 64) block(p);
    if (len) {
        std::memcpy(buf_, p, len);
        fill_ = len;
    }
}

std::array<u8, 20> Sha1::finish() {
    if (done_) fail(Fail::Generic, "SHA-1: finish called twice");
    done_ = true;
    const u64 bits = total_ * 8;
    buf_[fill_++] = 0x80;
    if (fill_ > 56) {
        std::memset(buf_ + fill_, 0, sizeof buf_ - fill_);
        block(buf_);
        fill_ = 0;
    }
    std::memset(buf_ + fill_, 0, 56 - fill_);
    store_be64(buf_ + 56, bits);
    block(buf_);
    std::array<u8, 20> out{};
    for (int i = 0; i < 5; ++i) store_be32(out.data() + 4 * i, h_[i]);
    return out;
}

std::array<u8, 32> sha256(const void* data, size_t len) {
    Sha256 h;
    h.update(data, len);
    return h.finish();
}

std::array<u8, 20> sha1(const void* data, size_t len) {
    Sha1 h;
    h.update(data, len);
    return h.finish();
}

}  // namespace nxs
