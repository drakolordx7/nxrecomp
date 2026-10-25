// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for nso.h: header parsing and build id, on a synthetic header and on the real main NSO when it is present.
#include "check.h"
#include "project_root.h"
#include "fileio.h"
#include "nso.h"
#include "storage.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mprs;

namespace {

const std::string kRealMainStr = mprs_test::project_path("game/exefs/main");
const char* kRealMain = kRealMainStr.c_str();
const char* kRealBuildId = "67C9CA1428222752F951021464A7B3597EB98FEC";

void put32(std::vector<u8>& v, size_t at, u32 x) {
    for (int i = 0; i < 4; ++i) v[at + i] = static_cast<u8>(x >> (8 * i));
}

// A 0x100-byte NSO header (plus `extra` zero bytes of body) with the given module id and sizes.
std::vector<u8> make_nso(const std::string& module_id_hex40, u32 text_c, u32 ro_c, u32 data_c, size_t extra) {
    std::vector<u8> v(0x100 + extra, 0);
    std::memcpy(v.data(), "NSO0", 4);
    put32(v, 4, 0);           // version
    put32(v, 0x0C, 0x3F);     // flags
    put32(v, 0x10 + 8, 0x1000);  // text decompressed size
    put32(v, 0x20 + 8, 0x800);   // ro decompressed size
    put32(v, 0x30 + 8, 0x400);   // data decompressed size
    put32(v, 0x3C, 0x2000);      // bss size
    // module id: first 20 bytes from the hex string, the rest is a pattern
    std::vector<u8> id(32);
    if (!hex_decode(module_id_hex40, id.data(), 20)) std::printf("FAIL test data: bad module id hex\n"), ++mprs_test::failures();
    for (size_t i = 20; i < 32; ++i) id[i] = static_cast<u8>(0xA0 + i);
    std::memcpy(v.data() + 0x40, id.data(), 32);
    put32(v, 0x60, text_c);
    put32(v, 0x64, ro_c);
    put32(v, 0x68, data_c);
    return v;
}

StoragePtr storage_of(std::vector<u8> bytes) { return make_memory_storage(std::move(bytes)); }

const std::string kZeroId(40, '0');

}  // namespace

int main() {
    // 1. Synthetic header: build id, flags and segment sizes come out as written.
    {
        const std::string id = "0123456789abcdef0011223344556677889900aa";   // 40 hex digits = the 20 bytes of the build id
        auto bytes = make_nso(id, 0x100, 0x80, 0x40, 0x100 + 0x80 + 0x40);
        NsoInfo info = nso_read_info(*storage_of(bytes));
        CHECK_EQ(info.build_id(), std::string("0123456789ABCDEF0011223344556677889900AA"));
        CHECK_EQ(info.flags, 0x3Fu);
        CHECK_EQ(info.text_size, 0x1000u);
        CHECK_EQ(info.ro_size, 0x800u);
        CHECK_EQ(info.data_size, 0x400u);
        CHECK_EQ(info.module_id[31], static_cast<u8>(0xA0 + 31));
    }

    // 2. Compressed sizes that end exactly at the end of the file are fine; one byte more is not.
    {
        auto exact = make_nso(kZeroId, 0x10, 0x20, 0x30, 0x10 + 0x20 + 0x30);
        CHECK_EQ(nso_read_info(*storage_of(exact)).text_size, 0x1000u);
        auto one_over = make_nso(kZeroId, 0x10, 0x20, 0x30, 0x10 + 0x20 + 0x30 - 1);
        CHECK_THROWS(nso_read_info(*storage_of(one_over)));
    }

    // 3. Too short: fewer than 0x100 bytes.
    {
        std::vector<u8> shorty(0xFF, 0);
        std::memcpy(shorty.data(), "NSO0", 4);
        CHECK_THROWS(nso_read_info(*storage_of(shorty)));
    }

    // 4. Bad magic.
    {
        auto bad = make_nso(kZeroId, 0, 0, 0, 0);
        bad[3] = '1';
        CHECK_THROWS(nso_read_info(*storage_of(bad)));
    }

    // 5. Compressed sizes that point past the end of the file.
    {
        auto big_text = make_nso(kZeroId, 0x10000, 0, 0, 0);
        CHECK_THROWS(nso_read_info(*storage_of(big_text)));
        auto big_ro = make_nso(kZeroId, 0, 0x7FFFFFFF, 0, 0);
        CHECK_THROWS(nso_read_info(*storage_of(big_ro)));
        auto big_data = make_nso(kZeroId, 0, 0, 0xFFFFFFFFu, 0);
        CHECK_THROWS(nso_read_info(*storage_of(big_data)));
    }

    // 6. The real main NSO, when it is present on this machine.
    if (!path_exists(kRealMain)) {
        std::printf("SKIP real NSO check: %s is not present\n", kRealMain);
    } else {
        auto file = open_file_storage(kRealMain);
        NsoInfo info = nso_read_info(*file);
        CHECK_EQ(info.build_id(), std::string(kRealBuildId));
    }

    return TEST_SUMMARY("test_nso");
}
