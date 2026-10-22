// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for ncz.h. The NCZ files are built here with the vendored zstd compressor (solid stream, two-frame solid stream,
// NCZBLOCK with 2^16 and 2^14 blocks, one block stored raw). The expected NCA bytes are computed here from the plain
// body, not by the reader, so a reader bug cannot agree with itself.
#include "check.h"
#include "crypto.h"
#include "ncz.h"
#include "storage.h"

#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace mprs;

namespace {

constexpr u64 kHdr = 0x4000;        // raw NCA header region
constexpr u64 kBodyStart = 0x4090;  // file offset of the compressed body: 0x4010 + 2 section entries of 0x40 bytes

// xorshift64: deterministic pseudo-random bytes.
struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    u64 next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    u8 byte() { return static_cast<u8>(next() >> 24); }
};

void store_le64(u8* p, u64 v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}
void store_be64(u8* p, u64 v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<u8>(v >> (56 - 8 * i));
}
void store_le32(u8* p, u32 v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}
u32 load_le32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }

void put_u64(std::vector<u8>& v, u64 x) {
    const size_t n = v.size();
    v.resize(n + 8);
    store_le64(v.data() + n, x);
}
void put_bytes(std::vector<u8>& v, const void* p, size_t n) {
    const u8* b = static_cast<const u8*>(p);
    v.insert(v.end(), b, b + n);
}

enum class Mode { Solid, SolidFrames, Block16, Block14 };
bool is_block(Mode m) { return m == Mode::Block16 || m == Mode::Block14; }
const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Solid: return "solid";
        case Mode::SolidFrames: return "solid-2frames";
        case Mode::Block16: return "block 2^16";
        case Mode::Block14: return "block 2^14";
    }
    return "?";
}

// The NCA as the NCZ stores it (header, optional plain gap, two sections) and the NCA that open_ncz must return.
struct Fixture {
    struct Layout {
        u64 gap, len0, len1;   // gap: plain bytes between the header and section 0
        u64 off0() const { return kHdr + gap; }
        u64 off1() const { return off0() + len0; }
        u64 end() const { return off1() + len1; }
    } lay{};
    u8 key[16]{}, ctr[16]{};       // section 0: AES-128-CTR (type 3)
    u8 key1[16]{}, ctr1[16]{};     // section 1: plain (type 1)
    std::vector<u8> plain;         // header + body as the NCZ holds them (section 0 decrypted)
    std::vector<u8> enc;           // what open_ncz must return (section 0 encrypted again)
};

// Pattern-heavy data (compressible) with random runs. The first 0x8000 bytes of the body are pattern only, so block 0
// always compresses.
std::vector<u8> make_body(Rng& r, size_t n) {
    std::vector<u8> v(n);
    size_t i = 0;
    while (i < n) {
        const bool head = i < 0x8000;
        const size_t run = std::min<size_t>(n - i, head ? 0x8000 - i : 1 + r.next() % 3000);
        const u64 kind = head ? 1 : r.next() % 3;
        for (size_t k = 0; k < run; ++k) {
            const size_t p = i + k;
            v[p] = kind == 0 ? r.byte() : (kind == 1 ? static_cast<u8>(p % 251) : static_cast<u8>('A' + p % 8));
        }
        i += run;
    }
    return v;
}

Fixture make_fixture(u64 gap, u64 seed) {
    Fixture f;
    f.lay = {gap, 0x12345, 0xF0F1};   // odd sizes: section 1 starts at a file offset that is not 16-byte aligned
    Rng r(seed);
    std::vector<u8> hdr(kHdr);
    for (auto& b : hdr) b = r.byte();
    for (auto& b : f.key) b = r.byte();
    for (auto& b : f.ctr) b = r.byte();
    for (auto& b : f.key1) b = r.byte();
    for (auto& b : f.ctr1) b = r.byte();
    const std::vector<u8> body = make_body(r, static_cast<size_t>(f.lay.end() - kHdr));
    f.plain = hdr;
    put_bytes(f.plain, body.data(), body.size());
    // Expected output: section 0 encrypted with counter = ctr[0..8] || big-endian64(offset / 16). Bytes 8..15 of the
    // stored counter are not used, as in nsz.
    f.enc = f.plain;
    u8 blk[16];
    std::memcpy(blk, f.ctr, 8);
    store_be64(blk + 8, f.lay.off0() >> 4);
    aes_ctr_crypt(f.key, blk, f.enc.data() + f.lay.off0(), static_cast<size_t>(f.lay.len0));
    return f;
}

std::vector<u8> zstd_frame(const u8* src, size_t n) {
    std::vector<u8> d(ZSTD_compressBound(n));
    const size_t c = ZSTD_compress(d.data(), d.size(), src, n, 5);
    if (ZSTD_isError(c)) throw std::runtime_error("ZSTD_compress failed");
    d.resize(c);
    return d;
}

// One zstd frame written with ZSTD_compressStream2 in irregular input chunks.
std::vector<u8> zstd_stream(const std::vector<u8>& in) {
    ZSTD_CCtx* c = ZSTD_createCCtx();
    if (!c) throw std::runtime_error("ZSTD_createCCtx failed");
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, 5);
    std::vector<u8> out(ZSTD_compressBound(in.size()) + 4096);
    size_t op = 0, ip = 0;
    Rng r(77);
    while (ip < in.size()) {
        const size_t n = std::min(in.size() - ip, static_cast<size_t>(1 + r.next() % 40000));
        ZSTD_inBuffer ib{in.data() + ip, n, 0};
        while (ib.pos < ib.size) {
            ZSTD_outBuffer ob{out.data() + op, out.size() - op, 0};
            const size_t rc = ZSTD_compressStream2(c, &ob, &ib, ZSTD_e_continue);
            op += ob.pos;
            if (ZSTD_isError(rc)) {
                ZSTD_freeCCtx(c);
                throw std::runtime_error("ZSTD_compressStream2 failed");
            }
        }
        ip += n;
    }
    for (;;) {
        ZSTD_outBuffer ob{out.data() + op, out.size() - op, 0};
        ZSTD_inBuffer ib{nullptr, 0, 0};
        const size_t rc = ZSTD_compressStream2(c, &ob, &ib, ZSTD_e_end);
        op += ob.pos;
        if (ZSTD_isError(rc)) {
            ZSTD_freeCCtx(c);
            throw std::runtime_error("ZSTD_compressStream2 end failed");
        }
        if (rc == 0) break;
    }
    ZSTD_freeCCtx(c);
    out.resize(op);
    return out;
}

void put_entry(std::vector<u8>& o, u64 offset, u64 size, u64 type, const u8 key[16], const u8 counter[16]) {
    put_u64(o, offset);
    put_u64(o, size);
    put_u64(o, type);
    put_u64(o, 0);   // padding
    put_bytes(o, key, 16);
    put_bytes(o, counter, 16);
}

// Builds the NCZ file for one body mode.
std::vector<u8> build_ncz(const Fixture& f, Mode m) {
    std::vector<u8> out(f.plain.begin(), f.plain.begin() + static_cast<std::ptrdiff_t>(kHdr));
    put_bytes(out, "NCZSECTN", 8);
    put_u64(out, 2);
    put_entry(out, f.lay.off0(), f.lay.len0, 3, f.key, f.ctr);
    put_entry(out, f.lay.off1(), f.lay.len1, 1, f.key1, f.ctr1);
    if (out.size() != kBodyStart) throw std::runtime_error("bad table size");

    const std::vector<u8> body(f.plain.begin() + static_cast<std::ptrdiff_t>(kHdr), f.plain.end());
    if (m == Mode::Solid) {
        const std::vector<u8> stream = zstd_stream(body);
        put_bytes(out, stream.data(), stream.size());
    } else if (m == Mode::SolidFrames) {
        // Two concatenated zstd frames split at an odd point.
        const size_t split = body.size() * 37 / 100 + 123;
        const std::vector<u8> a = zstd_frame(body.data(), split);
        const std::vector<u8> b = zstd_frame(body.data() + split, body.size() - split);
        put_bytes(out, a.data(), a.size());
        put_bytes(out, b.data(), b.size());
    } else {
        const u32 exp = (m == Mode::Block16) ? 16 : 14;
        const u64 raw_idx = (m == Mode::Block16) ? 1 : 2;   // this block is stored raw whatever its size
        const u64 bs = u64(1) << exp;
        const u64 n = (body.size() + bs - 1) / bs;
        std::vector<u8> data;
        std::vector<u32> csz;
        for (u64 i = 0; i < n; ++i) {
            const u64 dsize = std::min<u64>(bs, body.size() - i * bs);
            const u8* src = body.data() + i * bs;
            std::vector<u8> blk = zstd_frame(src, static_cast<size_t>(dsize));
            if (blk.size() >= dsize || i == raw_idx) blk.assign(src, src + dsize);
            csz.push_back(static_cast<u32>(blk.size()));
            data.insert(data.end(), blk.begin(), blk.end());
        }
        put_bytes(out, "NCZBLOCK", 8);
        out.push_back(2);   // version
        out.push_back(1);   // type
        out.push_back(0);   // unused
        out.push_back(static_cast<u8>(exp));
        const size_t at = out.size();
        out.resize(at + 4 + 8);
        store_le32(out.data() + at, static_cast<u32>(n));
        store_le64(out.data() + at + 4, body.size());
        for (u32 c : csz) {
            const size_t p = out.size();
            out.resize(p + 4);
            store_le32(out.data() + p, c);
        }
        put_bytes(out, data.data(), data.size());
    }
    return out;
}

StoragePtr mem(std::vector<u8> v) { return make_memory_storage(std::move(v)); }
std::vector<u8> cut(const std::vector<u8>& v, size_t n) { return std::vector<u8>(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n)); }

// Compares len bytes at off with the expected NCA. Any exception counts as a mismatch. Safe to call from several threads.
bool check_range(const IStorage& s, const std::vector<u8>& expect, u64 off, size_t len) {
    try {
        const std::vector<u8> got = read_all(s, off, len);
        return std::memcmp(got.data(), expect.data() + off, len) == 0;
    } catch (const std::exception&) {
        return false;
    }
}

void open_and_read_all(StoragePtr src) {
    StoragePtr nca = open_ncz(std::move(src));
    (void)read_all(*nca, 0, static_cast<size_t>(nca->size()));
}

std::vector<std::pair<u64, size_t>> special_reads(const Fixture& f, u64 bs, u64 size) {
    std::vector<std::pair<u64, size_t>> v = {
        {0x3FF9, 0x20},          // crosses the header / body boundary
        {0, 16},
        {f.lay.off0() - 3, 10},  // crosses into section 0
        {f.lay.off0() + 5, 33},  // misaligned start in section 0
        {f.lay.off1() - 5, 30},  // crosses section 0 -> section 1
        {f.lay.off1(), 1},
        {f.lay.off1() + 1, 17},
        {f.lay.end() - 40, 40},
        {f.lay.end() - 1, 1},
        {kHdr + 0x1234, 2},
        {f.lay.off0() + 7, 0x1FFFF},
    };
    for (u64 k = 1; k * bs <= size; ++k) {   // reads that straddle every block boundary
        const u64 start = k * bs - 7;
        if (start + 20 <= size) v.push_back({start, 20});
    }
    return v;
}

void variant_tests(const Fixture& f, Mode m) {
    const std::vector<u8>& expect = f.enc;
    const u64 size = expect.size();
    CHECK(expect != f.plain);   // the section really is re-encrypted
    StoragePtr nca = open_ncz(mem(build_ncz(f, m)));
    CHECK_EQ(nca->size(), size);

    CHECK(check_range(*nca, expect, 0, expect.size()));   // whole file in one read

    const u64 bs = (m == Mode::Block14) ? (u64(1) << 14) : (u64(1) << 16);
    for (const auto& [off, len] : special_reads(f, bs, size)) CHECK(check_range(*nca, expect, off, len));

    Rng r(1000 + static_cast<u64>(m) * 7 + f.lay.gap);
    for (int k = 0; k < 400; ++k) {
        const u64 off = r.next() % size;
        const size_t len = static_cast<size_t>(1 + r.next() % std::min<u64>(70000, size - off));
        CHECK(check_range(*nca, expect, off, len));
    }

    // Backwards: in solid mode this restarts the stream, in block mode it decodes from the cache or disk.
    const u64 probes[] = {size - 1000, 100, size / 2, 0x3000, size - 5000, 20};
    for (u64 p : probes) CHECK(check_range(*nca, expect, p, static_cast<size_t>(std::min<u64>(3000, size - p))));

    CHECK_THROWS(read_all(*nca, size - 1, 2));
    CHECK_THROWS(read_all(*nca, size, 1));

    // Four threads reading at random offsets at the same time.
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            Rng tr(5000 + static_cast<u64>(t));
            for (int k = 0; k < 150; ++k) {
                const u64 off = tr.next() % size;
                const size_t len = static_cast<size_t>(1 + tr.next() % std::min<u64>(60000, size - off));
                if (!check_range(*nca, expect, off, len)) ++bad;
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK_EQ(bad.load(), 0);
}

void negative_tests(const Fixture& f, Mode m) {
    const std::vector<u8> ncz = build_ncz(f, m);

    std::vector<u8> junk(0x4100);
    Rng r(5);
    for (auto& b : junk) b = r.byte();
    CHECK_THROWS(open_and_read_all(mem(junk)));                       // not an NCZ
    CHECK_THROWS(open_and_read_all(mem(cut(ncz, 100))));              // far too short
    CHECK_THROWS(open_and_read_all(mem(cut(ncz, 0x4050))));           // truncated inside the section table
    CHECK_THROWS(open_and_read_all(mem(cut(ncz, ncz.size() - 100)))); // truncated data

    {
        auto c = ncz;
        c[0x4000] = 'X';   // bad magic
        CHECK_THROWS(open_and_read_all(mem(c)));
    }
    {
        auto c = ncz;
        store_le64(c.data() + 0x4008, 0);   // zero sections
        CHECK_THROWS(open_and_read_all(mem(c)));
    }
    {
        auto c = ncz;
        store_le64(c.data() + 0x4008, 65);  // more than 64 sections
        CHECK_THROWS(open_and_read_all(mem(c)));
    }
    {
        auto c = ncz;
        store_le64(c.data() + 0x4050, f.lay.off1() + 0x100);   // section 1 points past where it must start
        CHECK_THROWS(open_and_read_all(mem(c)));
    }
    {
        auto c = ncz;
        store_le64(c.data() + 0x4010, 0x100);   // section 0 points into the header
        CHECK_THROWS(open_and_read_all(mem(c)));
    }
    {
        auto c = ncz;
        store_le64(c.data() + 0x4018, f.lay.len0 + 0x100);   // section 0 claims more bytes than the layout has
        CHECK_THROWS(open_and_read_all(mem(c)));
    }
    {
        auto c = ncz;
        store_le64(c.data() + 0x4058, ~u64(0));   // section 1 size overflows
        CHECK_THROWS(open_and_read_all(mem(c)));
    }

    if (is_block(m)) {
        {
            auto c = ncz;
            c[kBodyStart + 11] = 13;   // block size exponent out of range
            CHECK_THROWS(open_and_read_all(mem(c)));
        }
        {
            auto c = ncz;
            c[kBodyStart + 12] ^= 1;   // block count does not match the size
            CHECK_THROWS(open_and_read_all(mem(c)));
        }
        {
            auto c = ncz;
            const u32 nb = load_le32(c.data() + kBodyStart + 12);
            const size_t block0 = static_cast<size_t>(kBodyStart + 24 + 4ull * nb);   // block 0 is compressed
            c[block0] = c[block0 + 1] = c[block0 + 2] = c[block0 + 3] = 0;   // destroys its zstd frame magic
            CHECK_THROWS(open_and_read_all(mem(c)));
        }
    } else {
        auto c = ncz;
        c[kBodyStart] = c[kBodyStart + 1] = c[kBodyStart + 2] = c[kBodyStart + 3] = 0;   // destroys the zstd frame magic
        CHECK_THROWS(open_and_read_all(mem(c)));
    }
}

template <class F>
void run(const char* name, F fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        ++mprs_test::checks();
        ++mprs_test::failures();
        std::printf("FAIL %s: unexpected exception: %s\n", name, e.what());
    }
}

}  // namespace

int main() {
    for (u64 gap : {u64(0), u64(0x800)}) {
        const Fixture f = make_fixture(gap, 0xC0FFEE + gap);
        for (Mode m : {Mode::Solid, Mode::SolidFrames, Mode::Block16, Mode::Block14}) {
            std::printf("variant: gap 0x%llx, %s\n", static_cast<unsigned long long>(gap), mode_name(m));
            run("variant", [&] { variant_tests(f, m); });
            run("negatives", [&] { negative_tests(f, m); });
        }
    }
    return TEST_SUMMARY("test_zstd");
}
