// SPDX-License-Identifier: GPL-3.0-or-later
// AES-128 (ECB/CTR/XTS) and SHA-1/SHA-256 on top of Windows CNG (bcrypt.dll). No key bytes are ever logged.
#pragma once
#include "common.h"

namespace mprs {

// AES-128-ECB with one key. One object per thread (do not share an object across threads).
class Aes128Ecb {
public:
    explicit Aes128Ecb(const u8 key[16]);
    ~Aes128Ecb();
    Aes128Ecb(const Aes128Ecb&) = delete;
    Aes128Ecb& operator=(const Aes128Ecb&) = delete;
    // len must be a multiple of 16. in == out (in place) is allowed.
    void encrypt(const void* in, void* out, size_t len) const;
    void decrypt(const void* in, void* out, size_t len) const;
private:
    void* alg_ = nullptr;
    void* key_ = nullptr;
};

// AES-128-CTR in place. `counter` is the first 16-byte counter block; the whole block is a big-endian integer that is
// incremented once per 16 bytes. len may be any size (a partial last block uses the first bytes of the key work).
void aes_ctr_crypt(const u8 key[16], const u8 counter[16], void* data, size_t len);

// Nintendo's AES-128-XTS: standard XTS (two keys, GF(2^128) tweak update) except that the sector number is written into the
// 16-byte tweak input BIG-endian. `first_sector` is the number of the first sector of `data`; len is a multiple of sector_size.
void aes_xts_decrypt_nintendo(const u8 key1[16], const u8 key2[16], u64 first_sector, u32 sector_size, void* data, size_t len);
void aes_xts_encrypt_nintendo(const u8 key1[16], const u8 key2[16], u64 first_sector, u32 sector_size, void* data, size_t len);

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* data, size_t len);
    std::array<u8, 32> finish();   // after finish() the object is spent
private:
    void* alg_ = nullptr;
    void* hash_ = nullptr;
};
class Sha1 {
public:
    Sha1();
    ~Sha1();
    Sha1(const Sha1&) = delete;
    Sha1& operator=(const Sha1&) = delete;
    void update(const void* data, size_t len);
    std::array<u8, 20> finish();
private:
    void* alg_ = nullptr;
    void* hash_ = nullptr;
};
std::array<u8, 32> sha256(const void* data, size_t len);
std::array<u8, 20> sha1(const void* data, size_t len);

}  // namespace mprs
