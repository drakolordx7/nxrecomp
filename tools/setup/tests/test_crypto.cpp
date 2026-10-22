// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for crypto.h: AES-128 ECB / CTR / Nintendo XTS and SHA-1 / SHA-256 on Windows CNG.
#include "check.h"
#include "crypto.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

using namespace mprs;

namespace {

std::vector<u8> from_hex(const std::string& s) {
    std::vector<u8> out(s.size() / 2);
    if (!hex_decode(s, out.data(), out.size())) {
        std::printf("FAIL test data: bad hex\n");
        ++mprs_test::failures();
    }
    return out;
}

std::string hex(const std::vector<u8>& v) { return hex_encode(v.data(), v.size()); }

template <size_t N>
std::string hexa(const std::array<u8, N>& a) { return hex_encode(a.data(), N); }

std::array<u8, 16> arr16(const std::vector<u8>& v) {
    std::array<u8, 16> a{};
    std::memcpy(a.data(), v.data(), 16);
    return a;
}

// Adds n to a 128-bit big-endian counter (carry through all 16 bytes).
std::array<u8, 16> ctr_add(std::array<u8, 16> c, u64 n) {
    unsigned carry = 0;
    for (int i = 15; i >= 0; --i) {
        const unsigned add = static_cast<unsigned>(n & 0xff);
        n >>= 8;
        const unsigned v = c[i] + add + carry;
        c[i] = static_cast<u8>(v & 0xff);
        carry = v >> 8;
    }
    return c;
}

// Runs one test function; an exception escaping the library counts as a failure instead of aborting the run.
template <class F>
void run(const char* name, F f) {
    try {
        f();
    } catch (const std::exception& e) {
        ++mprs_test::checks();
        ++mprs_test::failures();
        std::printf("FAIL %s: unexpected exception: %s\n", name, e.what());
    }
}

// ---- AES-128 ECB -----------------------------------------------------------------------------------------------------

void test_ecb() {
    const std::vector<u8> key = from_hex("000102030405060708090a0b0c0d0e0f");
    const std::vector<u8> pt = from_hex("00112233445566778899aabbccddeeff");
    const Aes128Ecb aes(key.data());

    u8 ct[16], back[16];
    aes.encrypt(pt.data(), ct, 16);
    CHECK_EQ(hex_encode(ct, 16), std::string("69c4e0d86a7b0430d8cdb78070b4c55a"));
    aes.decrypt(ct, back, 16);
    CHECK(std::memcmp(back, pt.data(), 16) == 0);

    // in place
    u8 buf[16];
    std::memcpy(buf, pt.data(), 16);
    aes.encrypt(buf, buf, 16);
    CHECK_EQ(hex_encode(buf, 16), std::string("69c4e0d86a7b0430d8cdb78070b4c55a"));
    aes.decrypt(buf, buf, 16);
    CHECK(std::memcmp(buf, pt.data(), 16) == 0);

    // multi-block: one call equals block-by-block calls, in-place equals out-of-place, and decrypt inverts it
    std::vector<u8> multi(80);
    for (size_t i = 0; i < multi.size(); ++i) multi[i] = static_cast<u8>(i * 29 + 3);
    std::vector<u8> one(80), per(80), inplace = multi, rt(80);
    aes.encrypt(multi.data(), one.data(), 80);
    for (size_t b = 0; b < 5; ++b) aes.encrypt(&multi[16 * b], &per[16 * b], 16);
    CHECK(one == per);
    aes.encrypt(inplace.data(), inplace.data(), 80);
    CHECK(inplace == one);
    aes.decrypt(one.data(), rt.data(), 80);
    CHECK(rt == multi);
    // first block of a multi-block buffer that starts with the FIPS-197 plaintext
    std::vector<u8> fips_multi(pt);
    fips_multi.resize(32, 0xAA);
    std::vector<u8> fips_ct(32);
    aes.encrypt(fips_multi.data(), fips_ct.data(), 32);
    CHECK_EQ(hex_encode(fips_ct.data(), 16), std::string("69c4e0d86a7b0430d8cdb78070b4c55a"));

    // lengths that are not a multiple of 16 throw
    CHECK_THROWS(aes.encrypt(buf, buf, 17));
    CHECK_THROWS(aes.decrypt(buf, buf, 15));
    CHECK_THROWS(aes.encrypt(buf, buf, 1));
}

// ---- AES-128 CTR -----------------------------------------------------------------------------------------------------

void test_ctr() {
    const std::vector<u8> key = from_hex("2b7e151628aed2a6abf7158809cf4f3c");
    const std::vector<u8> ctr = from_hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    // NIST SP 800-38A F.5.1 CTR-AES128.Encrypt
    const std::vector<u8> pt = from_hex(
        "6bc1bee22e409f96e93d7e117393172a"
        "ae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52ef"
        "f69f2445df4f9b17ad2b417be66c3710");
    const std::string expect_hex =
        "874d6191b620e3261bef6864990db6ce"
        "9806f66b7970fdff8617187bb9fffdff"
        "5ae4df3edbd5d35e5b4f09020db03eab"
        "1e031dda2fbe03d1792170a0f3009cee";
    const std::vector<u8> expect = from_hex(expect_hex);

    std::vector<u8> d = pt;
    aes_ctr_crypt(key.data(), ctr.data(), d.data(), d.size());
    CHECK_EQ(hex(d), expect_hex);

    // encrypting again restores the plaintext
    aes_ctr_crypt(key.data(), ctr.data(), d.data(), d.size());
    CHECK(d == pt);

    // a 37-byte (partial block) call equals the first 37 bytes of the 48-byte result
    std::vector<u8> part(pt.begin(), pt.begin() + 37);
    aes_ctr_crypt(key.data(), ctr.data(), part.data(), 37);
    CHECK(part == std::vector<u8>(expect.begin(), expect.begin() + 37));

    // counter carry: 00..00 ffffffffffffffff, then ..0001 0000000000000000, then ..0001 0000000000000001
    std::array<u8, 16> cA{}, cB{}, cC{};
    for (int i = 8; i < 16; ++i) cA[i] = 0xff;
    cB[7] = 1;
    cC[7] = 1;
    cC[15] = 1;
    std::vector<u8> data48(48);
    for (size_t i = 0; i < 48; ++i) data48[i] = static_cast<u8>(i * 11 + 5);
    std::vector<u8> ref48(48);
    const std::array<std::array<u8, 16>, 3> counters = {cA, cB, cC};
    const Aes128Ecb aes(key.data());
    for (size_t b = 0; b < 3; ++b) {
        u8 ks[16];
        aes.encrypt(counters[b].data(), ks, 16);
        for (size_t i = 0; i < 16; ++i) ref48[16 * b + i] = static_cast<u8>(data48[16 * b + i] ^ ks[i]);
    }
    std::vector<u8> got48 = data48;
    aes_ctr_crypt(key.data(), cA.data(), got48.data(), got48.size());
    CHECK(got48 == ref48);

    // chunk boundary: 200000 bytes in one call equals two calls with the counter advanced by hand at a 16-byte multiple
    const size_t N = 200000, S = 100000;   // S = 6250 blocks
    std::vector<u8> big(N);
    for (size_t i = 0; i < N; ++i) big[i] = static_cast<u8>(i * 7 + i / 251);
    std::vector<u8> one_call = big;
    aes_ctr_crypt(key.data(), ctr.data(), one_call.data(), N);
    std::vector<u8> two_calls = big;
    aes_ctr_crypt(key.data(), ctr.data(), two_calls.data(), S);
    const std::array<u8, 16> ctr2 = ctr_add(arr16(ctr), S / 16);
    aes_ctr_crypt(key.data(), ctr2.data(), two_calls.data() + S, N - S);
    CHECK(one_call == two_calls);
    // the 65536-byte library chunk boundary (block 4096) also matches a direct block encryption
    const std::array<u8, 16> ctr4096 = ctr_add(arr16(ctr), 4096);
    u8 ks4096[16];
    aes.encrypt(ctr4096.data(), ks4096, 16);
    bool boundary_ok = true;
    for (size_t i = 0; i < 16; ++i)
        if (one_call[65536 + i] != static_cast<u8>(big[65536 + i] ^ ks4096[i])) boundary_ok = false;
    CHECK(boundary_ok);

    // speed (printed, not asserted): 256 MiB
    std::vector<u8> huge(size_t(256) << 20, 0x5a);
    const auto t0 = std::chrono::steady_clock::now();
    aes_ctr_crypt(key.data(), ctr.data(), huge.data(), huge.size());
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("AES-CTR speed: %.0f MB/s (%zu MiB in %.3f s)\n", (huge.size() / 1e6) / secs, huge.size() >> 20, secs);
    // round trip on the same buffer restores 0x5a everywhere
    aes_ctr_crypt(key.data(), ctr.data(), huge.data(), huge.size());
    CHECK(std::count(huge.begin(), huge.end(), static_cast<u8>(0x5a)) == static_cast<std::ptrdiff_t>(huge.size()));
}

// ---- AES-128 XTS (Nintendo) ------------------------------------------------------------------------------------------

// Independent reference: one block at a time, bytewise GF(2^128) multiply by x, using only Aes128Ecb.
void ref_xts_encrypt_sector(const Aes128Ecb& k1, const Aes128Ecb& k2, u64 sector, size_t ss, u8* buf) {
    u8 T[16] = {};
    for (int i = 0; i < 8; ++i) T[8 + i] = static_cast<u8>(sector >> (56 - 8 * i));   // Nintendo: big-endian sector
    k2.encrypt(T, T, 16);
    for (size_t j = 0; j < ss; j += 16) {
        u8 x[16], c[16];
        for (int i = 0; i < 16; ++i) x[i] = static_cast<u8>(buf[j + i] ^ T[i]);
        k1.encrypt(x, c, 16);
        for (int i = 0; i < 16; ++i) buf[j + i] = static_cast<u8>(c[i] ^ T[i]);
        u8 nt[16];
        for (int i = 0; i < 16; ++i) {
            const u8 prev = i == 0 ? 0 : static_cast<u8>(T[i - 1] >> 7);
            nt[i] = static_cast<u8>((T[i] << 1) | prev);
        }
        if (T[15] & 0x80) nt[0] ^= 0x87;
        std::memcpy(T, nt, 16);
    }
}

std::vector<u8> xts_pattern(size_t n) {
    std::vector<u8> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<u8>(i * 37 + 11);
    return v;
}

void test_xts() {
    const std::vector<u8> k1 = from_hex("2b7e151628aed2a6abf7158809cf4f3c");
    const std::vector<u8> k2 = from_hex("fedcba9876543210fedcba9876543210");
    const Aes128Ecb A(k1.data()), B(k2.data());

    struct Case { u64 first; u32 ss; size_t sectors; };
    const Case cases[] = {
        {0, 0x200, 1},
        {1, 0x200, 1},
        {0x1122334455667788ULL, 0x200, 1},
        {5, 0x200, 4},   // sectors 5..8 in one call
        {0x42, 16, 1},   // single block
    };
    for (const Case& c : cases) {
        const size_t len = size_t(c.ss) * c.sectors;
        std::vector<u8> plain = xts_pattern(len);
        std::vector<u8> ref = plain;
        for (size_t s = 0; s < c.sectors; ++s)
            ref_xts_encrypt_sector(A, B, c.first + s, c.ss, &ref[s * c.ss]);
        std::vector<u8> lib = plain;
        aes_xts_encrypt_nintendo(k1.data(), k2.data(), c.first, c.ss, lib.data(), len);
        CHECK(lib == ref);
        // decrypt inverts it
        aes_xts_decrypt_nintendo(k1.data(), k2.data(), c.first, c.ss, lib.data(), len);
        CHECK(lib == plain);
    }

    // length that is not a whole number of sectors throws
    std::vector<u8> odd(100, 0);
    CHECK_THROWS(aes_xts_encrypt_nintendo(k1.data(), k2.data(), 0, 0x20, odd.data(), odd.size()));

    // IEEE 1619 test vector 1: all-zero keys, sector 0, 32 zero bytes
    const std::vector<u8> zk(16, 0);
    std::vector<u8> z(32, 0);
    aes_xts_encrypt_nintendo(zk.data(), zk.data(), 0, 32, z.data(), z.size());
    CHECK_EQ(hex(z), std::string("917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e"));
    aes_xts_decrypt_nintendo(zk.data(), zk.data(), 0, 32, z.data(), z.size());
    CHECK(std::count(z.begin(), z.end(), static_cast<u8>(0)) == 32);
}

// ---- SHA-256 / SHA-1 -------------------------------------------------------------------------------------------------

void test_sha256() {
    CHECK_EQ(hexa(sha256("", 0)), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(hexa(sha256("abc", 3)), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const std::string million(1000000, 'a');
    const auto one_shot = sha256(million.data(), million.size());
    CHECK_EQ(hexa(one_shot), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

    // incremental, split at odd points, equals one-shot
    const std::vector<u8> data = xts_pattern(5000);
    const auto want = sha256(data.data(), data.size());
    Sha256 h;
    size_t off = 0;
    for (size_t step : {size_t(1), size_t(2), size_t(3), size_t(61), size_t(0), size_t(64), size_t(65), size_t(1000), size_t(2000)}) {
        h.update(data.data() + off, step);
        off += step;
    }
    h.update(data.data() + off, data.size() - off);
    CHECK(hexa(h.finish()) == hexa(want));

    // million 'a' fed in 1000-byte pieces
    Sha256 hm;
    for (size_t i = 0; i < million.size(); i += 1000) hm.update(million.data() + i, 1000);
    CHECK(hexa(hm.finish()) == hexa(one_shot));
}

void test_sha1() {
    CHECK_EQ(hexa(sha1("", 0)), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    CHECK_EQ(hexa(sha1("abc", 3)), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    const std::string million(1000000, 'a');
    const auto one_shot = sha1(million.data(), million.size());
    CHECK_EQ(hexa(one_shot), std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));

    const std::vector<u8> data = xts_pattern(5000);
    const auto want = sha1(data.data(), data.size());
    Sha1 h;
    size_t off = 0;
    for (size_t step : {size_t(1), size_t(2), size_t(3), size_t(61), size_t(0), size_t(64), size_t(65), size_t(1000), size_t(2000)}) {
        h.update(data.data() + off, step);
        off += step;
    }
    h.update(data.data() + off, data.size() - off);
    CHECK(hexa(h.finish()) == hexa(want));
}

}  // namespace

int main() {
    run("aes_ecb", test_ecb);
    run("aes_ctr", test_ctr);
    run("aes_xts", test_xts);
    run("sha256", test_sha256);
    run("sha1", test_sha1);
    return TEST_SUMMARY("test_crypto");
}
