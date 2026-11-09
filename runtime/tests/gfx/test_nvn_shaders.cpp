// SPDX-License-Identifier: GPL-3.0-or-later
// Shader cache (src/gfx/nvn/nvn_shaders.{h,cpp}): SHA-1 vectors, the control section and the cache's failure path. The cases that look a game's
// whole shader corpus up through the cache are the game's (its tests/test_nvn_shaders_corpus.cpp).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "nvn_shaders.h"
#include "test_framework.h"
#include "project_root.h"
NXTEST_GROUP(gfx_nvn_shaders)

using namespace nxrt::nvn::be;

namespace {

// The control section of a block as the corpus files hold it (0x880 bytes), zeroed except what is set.
void make_control(uint8_t* ctrl, uint32_t code_size, uint32_t const_offset, uint32_t const_size) {
    std::memset(ctrl, 0, 0x880);
    auto put = [&](size_t off, uint32_t v) { std::memcpy(ctrl + off, &v, 4); };
    put(0, kShaderControlMagic);
    put(0x6f8, 0x100);
    put(0x6fc, const_size);
    put(0x700, const_offset);
    put(0x704, code_size);
    put(0x708, 0x30);
    put(0x748, 8);
    put(0x74c, 4);
    put(0x750, 2);
}

}  // namespace


NXTEST(sha1_standard_vectors) {
    CHECK_EQ(sha1_hex("", 0), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    CHECK_EQ(sha1_hex("abc", 3), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    const std::string two_block = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK_EQ(sha1_hex(two_block.data(), two_block.size()), std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    // One million 'a', fed in uneven pieces to exercise the buffering.
    Sha1 s;
    std::string a(1000000, 'a');
    size_t off = 0, step = 1;
    while (off < a.size()) {
        size_t n = std::min(step, a.size() - off);
        s.update(a.data() + off, n);
        off += n;
        step = step * 7 % 131 + 1;
    }
    uint8_t d[20];
    s.finish(d);
    char hex[41];
    for (int i = 0; i < 20; i++) std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
    CHECK_EQ(std::string(hex), std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

// Numbers in a harness record that do not fit an int are refused (the parser used a 32-bit long and overflowed before its range check).
NXTEST(harness_numbers_out_of_range) {
    std::vector<HarnessResource> r;
    CHECK(parse_harness_resources("cb2147483647=s0b1", r));
    CHECK_EQ(r.size(), size_t(1));
    if (!r.empty()) CHECK_EQ(r[0].bank, 2147483647);
    CHECK(!parse_harness_resources("cb2147483648=s0b1", r));
    CHECK(!parse_harness_resources("cb4294967297=s0b1", r));      // wrapped to 1 with a 32-bit accumulator
    CHECK(!parse_harness_resources("cb99999999999=s0b1", r));
    CHECK(!parse_harness_resources("sb[c0+0x100000000]=s0b1", r));
}

NXTEST(shader_hash_is_sha1_prefix) {
    CHECK_EQ(shader_hash(reinterpret_cast<const uint8_t*>("abc"), 3), std::string("a9993e364706816a"));
}

NXTEST(read_control_fields) {
    uint8_t ctrl[0x880];
    make_control(ctrl, 0x1400, 0x1300, 0x18);
    ShaderControl c = read_control(ctrl);
    CHECK(c.valid);
    CHECK_EQ(c.code_size, 0x1400u);
    CHECK_EQ(c.const_offset, 0x1300u);
    CHECK_EQ(c.const_size, 0x18u);
    CHECK_EQ(c.program_start, 0x30u);
    CHECK_EQ(c.program_size, 0x100u);
    CHECK_EQ(c.local_size[0], 8u);
    CHECK_EQ(c.local_size[1], 4u);
    CHECK_EQ(c.local_size[2], 2u);
    std::memset(ctrl, 0, sizeof ctrl);
    CHECK(!read_control(ctrl).valid);
}

NXTEST(cache_missing_directory_fails_quietly) {
    ShaderCache c;
    CHECK(!c.open("D:/no/such/shader/cache"));
    CHECK(!c.is_open());
    uint8_t code[64] = {}, ctrl[0x880];
    make_control(ctrl, sizeof code, 0, 0);
    CHECK(c.find(code, ctrl) == nullptr);
    CHECK(c.find_by_hash("0123456789abcdef") == nullptr);
    CHECK_EQ(c.loaded(), size_t(0));
    CHECK_EQ(c.missing(), size_t(0));
}

NXTEST(cache_rejects_malformed_control) {
    ShaderCache c;
    uint8_t code[64] = {}, ctrl[0x880] = {};  // no magic
    CHECK(c.find(code, ctrl) == nullptr);
    CHECK(c.find(nullptr, ctrl) == nullptr);
}
