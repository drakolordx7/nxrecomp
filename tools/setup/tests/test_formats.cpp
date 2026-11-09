// SPDX-License-Identifier: GPL-3.0-or-later
// formats/nx_formats.h, the file formats shared with the runtime: the hash and the BC7 cache key against the vectors formats/test_formats.py
// holds for the Python twin, the cache header, programs.bin (the neutral magic, the first Metroid release's magic, refusals) and the pack magics.
#include <cstring>
#include <vector>

#include "check.h"
#include "nx_formats.h"

using namespace nxfmt;

namespace {

std::vector<uint8_t> vector_of(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 7 + 3);
    return v;
}

void test_hash_and_key() {
    // The same numbers as VECTOR_HASH / VECTOR_KEY in formats/test_formats.py (computed by the Python twin).
    CHECK_EQ(hash_bytes(vector_of(75).data(), 75), 0xa0bd9a2db08e831full);
    Bc7Meta m;
    m.nvn_format = 0x7F;
    m.width = 8;
    m.height = 8;
    m.depth = 1;
    m.levels = 4;
    m.layers = 1;
    m.block_height = 1;
    m.pitch = 0;
    m.quality = 3;
    CHECK_EQ(bc7_cache_key(vector_of(64).data(), 64, m), 0xa6f65dbf8e2792edull);
    CHECK_EQ(hash_bytes(nullptr, 0), hash_seeded(nullptr, 0, 0));
    CHECK_EQ(bc7_cache_rel_path(0x0123456789abcdefull), std::string("01/0123456789abcdef.bc7"));
}

void test_cache_head() {
    std::vector<uint8_t> out;
    const Bc7Level lv[2] = {{8, 8, 1, 0, 64}, {4, 4, 1, 64, 16}};
    bc7_put_head(out, 0x1122334455667788ull, 145, 1, lv, 2);
    CHECK_EQ(out.size(), kBc7HeadBytes + 2 * kBc7LevelBytes);
    CHECK(bc7_head_matches(out.data(), out.size(), 0x1122334455667788ull));
    CHECK(!bc7_head_matches(out.data(), out.size(), 1));
    CHECK(!bc7_head_matches(out.data(), 10, 0x1122334455667788ull));
}

void test_programs() {
    const std::vector<ProgramHashes> progs = {{1, 2, 0}, {1, 3, 0}, {5, 1, 9}};
    std::vector<uint8_t> b = programs_encode(progs);
    CHECK_EQ(b.size(), kProgramsHeadBytes + 3 * kProgramsRecordBytes);
    CHECK(std::memcmp(b.data(), "NXPRG002", 8) == 0);
    std::vector<ProgramHashes> back;
    CHECK(programs_decode(b.data(), b.size(), back) == nullptr);
    CHECK(back == progs);
    // the first Metroid release's magic is still read
    std::memcpy(b.data(), "MPRPRG01", 8);
    CHECK(programs_decode(b.data(), b.size(), back) == nullptr);
    CHECK(back == progs);
    std::vector<uint8_t> bad = b;
    bad[0] = 'X';
    CHECK(programs_decode(bad.data(), bad.size(), back) != nullptr);
    CHECK(programs_decode(b.data(), b.size() - 1, back) != nullptr);
    CHECK(programs_decode(b.data(), 8, back) != nullptr);
    bad = b;
    bad[12] = 1;   // reserved word
    CHECK(programs_decode(bad.data(), bad.size(), back) != nullptr);
    const std::vector<uint8_t> unsorted = programs_encode({{2, 0, 0}, {1, 0, 0}});
    CHECK(programs_decode(unsorted.data(), unsorted.size(), back) != nullptr);
    const std::vector<uint8_t> empty = programs_encode({});
    CHECK(programs_decode(empty.data(), empty.size(), back) == nullptr);
    CHECK(back.empty());
}

void test_pack_magics() {
    CHECK(shader_pack_magic_ok("NXSHC002"));
    CHECK(shader_pack_magic_ok("MPRSHC01"));
    CHECK(!shader_pack_magic_ok("NXSHC001"));
}

}  // namespace

int main() {
    test_hash_and_key();
    test_cache_head();
    test_programs();
    test_pack_magics();
    return TEST_SUMMARY("test_formats");
}
