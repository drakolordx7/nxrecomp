// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for programs.h: the split, hash and program rules on a synthetic program set; the embedded-set scanner (the zlib stream must end
// exactly at its declared length); the programs.bin round trip and refusals. With MPRS_ROMFS set, also a real scan (and, with
// MPRS_PROGRAMS_REF set, a byte-for-byte comparison of the written file against that reference).
#include "check.h"
#include "crypto.h"
#include "fileio.h"
#include "programs.h"
#include "storage.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../third_party/miniz/miniz.h"   // not the older copy in textures/bc7enc_rdo

using namespace mprs;
namespace fs = std::filesystem;

namespace {

constexpr size_t kCode = 0x100;   // size of every synthetic code block
constexpr size_t kCtrl = 0x880;   // size of every synthetic control section

// One inflated program set: one code block and one control section per entry of stages (NVN stage numbers: 0 and 1 vertex,
// 2 tessellation control, 3 tessellation evaluation, 4 geometry, 5 fragment, 9 unknown). Code blocks come first, then control sections.
std::vector<u8> make_set(const std::vector<u32>& stages, const std::vector<u8>& fill) {
    const size_t n = stages.size();
    std::vector<u8> raw(n * (kCode + kCtrl), 0);
    for (size_t k = 0; k < n; ++k) {
        u8* code = raw.data() + k * kCode;
        u8* ctrl = raw.data() + n * kCode + k * kCtrl;
        code[0] = 0x78; code[1] = 0x56; code[2] = 0x34; code[3] = 0x12;   // code magic
        const u32 word = stages[k] << 10;
        for (int i = 0; i < 4; ++i) code[0x30 + i] = u8(word >> (8 * i));   // program header: stage field at start = 0x30
        for (size_t i = 0x40; i < kCode; ++i) code[i] = u8((i * 7) ^ fill[k]);
        ctrl[0] = 0x34; ctrl[1] = 0x12; ctrl[2] = 0x76; ctrl[3] = 0x98;   // control magic
        ctrl[0x704] = 0x00; ctrl[0x705] = 0x01;                            // code block size = 0x100
        ctrl[0x708] = 0x30;                                                // program header offset
    }
    return raw;
}

// The hash the runtime uses for block k: first 8 bytes of SHA-1 of its code bytes, big-endian.
u64 hash_block(const std::vector<u8>& raw, size_t k) {
    const std::array<u8, 20> d = sha1(raw.data() + k * kCode, kCode);
    u64 h = 0;
    for (int i = 0; i < 8; ++i) h = (h << 8) | d[i];
    return h;
}

std::vector<u8> le32(u32 v) {
    return {u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24)};
}

// A decompressed program set as it sits inside an MTRL blob: 00 xx xx xx xx | 11 00 00 00 | u32 zlibLen | zlib stream | tail.
std::vector<u8> embed(const std::vector<u8>& zlib, u32 declared_len, const std::vector<u8>& tail) {
    std::vector<u8> b = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x00, 0x01, 0x02, 0x03, 0x04, 0x11, 0x00, 0x00, 0x00};
    for (u8 v : le32(declared_len)) b.push_back(v);
    b.insert(b.end(), zlib.begin(), zlib.end());
    b.insert(b.end(), tail.begin(), tail.end());
    return b;
}

std::vector<u8> zlib_of(const std::vector<u8>& raw) {
    size_t n = 0;
    const int flags = static_cast<int>(TDEFL_WRITE_ZLIB_HEADER) | static_cast<int>(TDEFL_DEFAULT_MAX_PROBES);
    void* p = tdefl_compress_mem_to_heap(raw.data(), raw.size(), &n, flags);
    std::vector<u8> out(static_cast<u8*>(p), static_cast<u8*>(p) + n);
    mz_free(p);
    return out;
}

std::vector<u8> read_bytes(const std::string& path) {
    std::ifstream f(fs::path(widen(path)), std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void write_bytes(const std::string& path, const std::vector<u8>& b) {
    std::ofstream f(fs::path(widen(path)), std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

}  // namespace

int main() {
    // ---- 1. split, hash and program rules on synthetic sets ----------------------------------------------------------------
    {
        ProgramHashes p;
        std::vector<u8> raw = make_set({0, 5}, {0x11, 0x22});
        CHECK(program_of_raw_set(raw, p) == SetResult::Program);
        CHECK_EQ(p.vs, hash_block(raw, 0));
        CHECK_EQ(p.fs, hash_block(raw, 1));
        CHECK_EQ(p.gs, u64(0));
        // The hash is the first 16 hex digits of the SHA-1 of the code bytes (the same text as the cache file names).
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(p.vs));
        const std::array<u8, 20> d = sha1(raw.data(), kCode);
        CHECK(hex_encode(d.data(), 8) == std::string(hex));

        // Vertex stage 1 is also vertex; a geometry block sits between the vertex and fragment blocks.
        raw = make_set({1, 4, 5}, {0x01, 0x02, 0x03});
        CHECK(program_of_raw_set(raw, p) == SetResult::Program);
        CHECK_EQ(p.vs, hash_block(raw, 0));
        CHECK_EQ(p.gs, hash_block(raw, 1));
        CHECK_EQ(p.fs, hash_block(raw, 2));

        // An unknown stage field is ignored (and counted).
        u32 unknown = 0;
        raw = make_set({0, 9, 5}, {0x04, 0x05, 0x06});
        CHECK(program_of_raw_set(raw, p, &unknown) == SetResult::Program);
        CHECK_EQ(unknown, 1u);
        CHECK_EQ(p.gs, u64(0));

        // Skipped: compute (first word 0x12345679), tessellation, repeated stage, no fragment.
        raw = make_set({0, 5}, {0x07, 0x08});
        raw[0] = 0x79;   // 0x12345679: compute
        CHECK(program_of_raw_set(raw, p) == SetResult::Skipped);
        CHECK(program_of_raw_set(make_set({0, 2, 5}, {1, 2, 3}), p) == SetResult::Skipped);
        CHECK(program_of_raw_set(make_set({0, 1, 5}, {1, 2, 3}), p) == SetResult::Skipped);
        CHECK(program_of_raw_set(make_set({0}, {1}), p) == SetResult::Skipped);

        // Invalid: control section size does not match the code block, an unpaired control section, a header offset past the code.
        // Two sets of 2 stages: code blocks at 0x000 and 0x100, control sections at 0x200 (first) and 0x200 + kCtrl (second).
        const size_t ctrl0 = 2 * kCode, ctrl1 = 2 * kCode + kCtrl;
        raw = make_set({0, 5}, {0x09, 0x0a});
        raw[ctrl1 + 0x704] = 0x01;       // second control section names code size 0x101
        CHECK(program_of_raw_set(raw, p) == SetResult::Invalid);
        raw = make_set({0, 5}, {0x09, 0x0a});
        raw[ctrl1] = 0;                  // the second control magic is gone: 2 code blocks, 1 control section
        CHECK(program_of_raw_set(raw, p) == SetResult::Invalid);
        raw = make_set({0, 5}, {0x09, 0x0a});
        raw[ctrl0 + 0x708] = 0xF0;       // program header offset 0xfff0: outside the 0x100-byte code block
        raw[ctrl0 + 0x709] = 0xFF;
        CHECK(program_of_raw_set(raw, p) == SetResult::Invalid);
        CHECK(program_of_raw_set(std::vector<u8>(64, 0), p) == SetResult::Invalid);
    }

    // ---- 2. embedded sets: the zlib stream must end exactly at its declared length -------------------------------------------
    {
        const std::vector<u8> raw = make_set({0, 5}, {0x31, 0x32});
        const std::vector<u8> z = zlib_of(raw);
        CHECK(z.size() > 2 && z[0] == 0x78);

        auto sets = embedded_raw_sets(embed(z, u32(z.size()), {0x00, 0x00}));
        CHECK_EQ(sets.size(), size_t(1));
        if (sets.size() == 1) CHECK(sets[0] == raw);

        // The declared length is one byte longer and a junk byte follows the stream: the stream does not end at its declared length.
        sets = embedded_raw_sets(embed(z, u32(z.size() + 1), {0x00, 0x00}));
        CHECK_EQ(sets.size(), size_t(0));

        // Declared length shorter than the stream: it does not complete.
        sets = embedded_raw_sets(embed(z, u32(z.size() - 1), {0x00, 0x00}));
        CHECK_EQ(sets.size(), size_t(0));

        // Inflates, but does not start with a code magic.
        const std::vector<u8> plain = zlib_of(std::vector<u8>(64, 0x41));
        sets = embedded_raw_sets(embed(plain, u32(plain.size()), {0x00, 0x00}));
        CHECK_EQ(sets.size(), size_t(0));

        // Output larger than the inflater's first buffer (the buffer must grow); the trailing zeros do not change the split.
        std::vector<u8> big = raw;
        big.resize(big.size() + 200000, 0);
        const std::vector<u8> zb = zlib_of(big);
        sets = embedded_raw_sets(embed(zb, u32(zb.size()), {0x00, 0x00}));
        CHECK_EQ(sets.size(), size_t(1));
        if (sets.size() == 1) CHECK(sets[0] == big);
        ProgramHashes pb;
        CHECK(program_of_raw_set(big, pb) == SetResult::Program);
        CHECK_EQ(pb.vs, hash_block(raw, 0));
    }

    // ---- 3. programs.bin: round trip, empty list, refusals --------------------------------------------------------------------
    const fs::path tmp = fs::temp_directory_path() /
                         (L"mprs_test_programs_" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tmp);
    const std::string dir = narrow(tmp.wstring());
    const std::string file = path_join(dir, "programs.bin");
    {
        const std::vector<ProgramHashes> list = {{1, 2, 0}, {1, 2, 3}, {0x0123456789abcdefull, 0xffffffffffffffffull, 7}};
        write_programs_file(file, list);
        const std::vector<u8> b = read_bytes(file);
        CHECK_EQ(b.size(), size_t(16 + 24 * 3));
        CHECK(std::memcmp(b.data(), "MPRPRG01", 8) == 0);
        CHECK_EQ(b[8], u8(3));
        CHECK_EQ(b[12], u8(0));
        CHECK(read_programs_file(file) == list);

        write_programs_file(file, {});
        CHECK_EQ(read_bytes(file).size(), size_t(16));
        CHECK(read_programs_file(file).empty());

        // Refusals: truncated, wrong magic, nonzero reserved word, records out of order.
        write_programs_file(file, list);
        std::vector<u8> good = read_bytes(file);
        std::vector<u8> cut(good.begin(), good.end() - 1);
        write_bytes(file, cut);
        CHECK_THROWS(read_programs_file(file));
        std::vector<u8> magic = good;
        magic[0] = 'X';
        write_bytes(file, magic);
        CHECK_THROWS(read_programs_file(file));
        std::vector<u8> reserved = good;
        reserved[12] = 1;
        write_bytes(file, reserved);
        CHECK_THROWS(read_programs_file(file));
        write_programs_file(file, {{1, 2, 3}, {1, 2, 0}});
        CHECK_THROWS(read_programs_file(file));
        CHECK_THROWS(read_programs_file(path_join(dir, "missing.bin")));
    }

    // ---- 4. real data (only with MPRS_ROMFS) -----------------------------------------------------------------------------------
    if (const char* romfs = std::getenv("MPRS_ROMFS")) {
        ProgramScanStats st;
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<ProgramHashes> progs =
            scan_shader_programs(romfs, 0, [](u64, u64) {}, [] { return false; }, &st);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("real: %zu programs in %.1f s\n", progs.size(), secs);
        std::printf("  arc: sets %u, invalid %u, skipped %u, programs %u, inflate failed %u\n", st.arc.units, st.arc.invalid,
                    st.arc.skipped, st.arc.programs, st.arc_inflate_failed);
        std::printf("  paks: MTRL assets %u (failed %u), sets %u, invalid %u, skipped %u, programs %u, pak files skipped %u\n",
                    st.mtrl_assets, st.mtrl_failed, st.pak.units, st.pak.invalid, st.pak.skipped, st.pak.programs,
                    st.pak_files_skipped);
        std::printf("  unknown-stage blocks %u, unique %u\n", st.unknown_stage_blocks, st.unique_programs);
        if (!st.first_asset_error.empty()) std::printf("  first MTRL failure: %s\n", st.first_asset_error.c_str());
        // MPRS_PROGRAMS_OUT keeps the written file (otherwise it is deleted with the temp folder).
        const std::string out = std::getenv("MPRS_PROGRAMS_OUT") ? std::getenv("MPRS_PROGRAMS_OUT") : path_join(dir, "programs_real.bin");
        write_programs_file(out, progs);
        CHECK_EQ(read_programs_file(out).size(), progs.size());
        if (const char* ref_path = std::getenv("MPRS_PROGRAMS_REF")) {
            const std::vector<u8> mine = read_bytes(out);
            const std::vector<u8> ref = read_bytes(ref_path);
            const bool same = mine == ref;
            std::printf("  reference: %s (%zu bytes, mine %zu bytes)\n", same ? "identical" : "DIFFERENT", ref.size(), mine.size());
            CHECK(same);
            if (!same) {
                const size_t n = std::min(mine.size(), ref.size());
                size_t i = 16;
                while (i + 24 <= n && std::memcmp(&mine[i], &ref[i], 24) == 0) i += 24;
                if (i + 24 <= n || mine.size() != ref.size()) {
                    const size_t rec = (i - 16) / 24;
                    std::printf("  first difference at record %zu (file offset %zu)\n", rec, i);
                    for (int k = 0; k < 3 && i + 8 * (k + 1) <= n; ++k) {
                        u64 a = 0, b2 = 0;
                        for (int j = 7; j >= 0; --j) {
                            a = (a << 8) | mine[i + 8 * k + j];
                            b2 = (b2 << 8) | ref[i + 8 * k + j];
                        }
                        std::printf("    field %d: mine %016llx ref %016llx\n", k, static_cast<unsigned long long>(a),
                                    static_cast<unsigned long long>(b2));
                    }
                }
            }
        }
    } else {
        std::printf("real data: skipped (MPRS_ROMFS not set)\n");
    }

    std::error_code ec;
    fs::remove_all(tmp, ec);
    return TEST_SUMMARY("programs");
}
