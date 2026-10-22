// SPDX-License-Identifier: GPL-3.0-or-later
// AES-128 (ECB/CTR/XTS) and SHA-1/SHA-256 on top of Windows CNG (bcrypt.dll). No key bytes are ever logged.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <stdlib.h>   // _byteswap_uint64

#include "crypto.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace mprs {
namespace {

// Throws mprs::Error. The message carries only the CNG status code, never key or data bytes.
[[noreturn]] void cng_fail(const char* what, NTSTATUS st) {
    fail(Fail::Generic, "%s (Windows crypto status 0x%08lX)", what, static_cast<unsigned long>(st));
}

// dst[i] ^= src[i], 64-bit words where possible. memcpy keeps it free of alignment and aliasing UB.
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

// Big-endian 64-bit load/store. Windows on x64 is little-endian, so a byte swap is the conversion.
u64 load_be64(const u8* p) {
    u64 v;
    std::memcpy(&v, p, 8);
    return _byteswap_uint64(v);
}
void store_be64(u8* p, u64 v) {
    v = _byteswap_uint64(v);
    std::memcpy(p, &v, 8);
}

// One ECB call per chunk of at most 1 GiB (BCrypt takes ULONG sizes). in == out is allowed.
constexpr size_t kMaxCall = size_t(1) << 30;

void aes_ecb_call(void* key, const void* in, void* out, size_t len, bool encrypt) {
    if (len % 16 != 0) fail(Fail::Generic, "AES: %zu bytes is not a multiple of 16", len);
    const u8* src = static_cast<const u8*>(in);
    u8* dst = static_cast<u8*>(out);
    while (len > 0) {
        const ULONG n = static_cast<ULONG>(std::min(len, kMaxCall));
        ULONG written = 0;
        NTSTATUS st = encrypt
            ? BCryptEncrypt(key, const_cast<PUCHAR>(src), n, nullptr, nullptr, 0, dst, n, &written, 0)
            : BCryptDecrypt(key, const_cast<PUCHAR>(src), n, nullptr, nullptr, 0, dst, n, &written, 0);
        if (!BCRYPT_SUCCESS(st)) cng_fail(encrypt ? "AES encryption failed" : "AES decryption failed", st);
        if (written != n) fail(Fail::Generic, "AES: wrote %lu of %lu bytes", written, n);
        src += n;
        dst += n;
        len -= n;
    }
}

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

void hash_update(void* hash, const void* data, size_t len) {
    const u8* p = static_cast<const u8*>(data);
    while (len > 0) {
        const ULONG n = static_cast<ULONG>(std::min(len, kMaxCall));
        NTSTATUS st = BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash), const_cast<PUCHAR>(p), n, 0);
        if (!BCRYPT_SUCCESS(st)) cng_fail("Hashing failed", st);
        p += n;
        len -= n;
    }
}

}  // namespace

// ---- AES-128-ECB -----------------------------------------------------------------------------------------------------

Aes128Ecb::Aes128Ecb(const u8 key[16]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE kh = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (BCRYPT_SUCCESS(st)) {
        st = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                               reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(BCRYPT_CHAIN_MODE_ECB)),
                               sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
        if (BCRYPT_SUCCESS(st)) {
            st = BCryptGenerateSymmetricKey(alg, &kh, nullptr, 0, const_cast<PUCHAR>(key), 16, 0);
        }
    }
    if (!BCRYPT_SUCCESS(st)) {
        if (kh) BCryptDestroyKey(kh);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        cng_fail("AES setup failed", st);
    }
    alg_ = alg;
    key_ = kh;
}

Aes128Ecb::~Aes128Ecb() {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
    if (alg_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg_), 0);
}

void Aes128Ecb::encrypt(const void* in, void* out, size_t len) const {
    aes_ecb_call(key_, in, out, len, true);
}

void Aes128Ecb::decrypt(const void* in, void* out, size_t len) const {
    aes_ecb_call(key_, in, out, len, false);
}

// ---- AES-128-CTR -----------------------------------------------------------------------------------------------------

void aes_ctr_crypt(const u8 key[16], const u8 counter[16], void* data, size_t len) {
    if (len == 0) return;
    Aes128Ecb aes(key);
    constexpr size_t kChunk = 64 * 1024;   // multiple of 16; keystream for one chunk is built and encrypted in one call
    std::vector<u8> ks(kChunk);
    u64 hi = load_be64(counter);       // the counter block as one 128-bit big-endian integer: hi = bytes 0..7
    u64 lo = load_be64(counter + 8);   //                                                       lo = bytes 8..15
    u8* p = static_cast<u8*>(data);
    while (len > 0) {
        const size_t n = std::min(len, kChunk);
        const size_t blocks = (n + 15) / 16;
        for (size_t b = 0; b < blocks; ++b) {
            store_be64(&ks[b * 16], hi);
            store_be64(&ks[b * 16 + 8], lo);
            if (++lo == 0) ++hi;   // carry from the low half into the high half
        }
        aes.encrypt(ks.data(), ks.data(), blocks * 16);
        xor_into(p, ks.data(), n);   // a partial last block only uses the first n % 16 key stream bytes
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
    std::vector<u8> tw(sector_size);   // T_j for every block of the current sector
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

Sha256::Sha256() {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (BCRYPT_SUCCESS(st)) st = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(st)) {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        cng_fail("SHA-256 setup failed", st);
    }
    alg_ = alg;
    hash_ = h;
}

Sha256::~Sha256() {
    if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    if (alg_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg_), 0);
}

void Sha256::update(const void* data, size_t len) {
    if (!hash_) fail(Fail::Generic, "SHA-256: update after finish");
    hash_update(hash_, data, len);
}

std::array<u8, 32> Sha256::finish() {
    if (!hash_) fail(Fail::Generic, "SHA-256: finish called twice");
    std::array<u8, 32> out{};
    NTSTATUS st = BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), out.data(), 32, 0);
    BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    hash_ = nullptr;   // spent
    if (!BCRYPT_SUCCESS(st)) cng_fail("SHA-256 finish failed", st);
    return out;
}

Sha1::Sha1() {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
    if (BCRYPT_SUCCESS(st)) st = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(st)) {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        cng_fail("SHA-1 setup failed", st);
    }
    alg_ = alg;
    hash_ = h;
}

Sha1::~Sha1() {
    if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    if (alg_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg_), 0);
}

void Sha1::update(const void* data, size_t len) {
    if (!hash_) fail(Fail::Generic, "SHA-1: update after finish");
    hash_update(hash_, data, len);
}

std::array<u8, 20> Sha1::finish() {
    if (!hash_) fail(Fail::Generic, "SHA-1: finish called twice");
    std::array<u8, 20> out{};
    NTSTATUS st = BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), out.data(), 20, 0);
    BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    hash_ = nullptr;   // spent
    if (!BCRYPT_SUCCESS(st)) cng_fail("SHA-1 finish failed", st);
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

}  // namespace mprs
