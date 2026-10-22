// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the RomFS parser (src/romfs.cpp) on synthetic images from synth.cpp.
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "common.h"
#include "romfs.h"
#include "synth.h"

using namespace mprs;
using mprs_test::SynthFiles;
using mprs_test::synth_romfs;

namespace {

constexpr u64 kNotFound = ~u64(0);

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 5 + (i >> 8));
    return v;
}

StoragePtr mem(std::vector<uint8_t> b) { return make_memory_storage(std::move(b)); }

template <class F>
bool throws_input(F fn) {
    try {
        fn();
    } catch (const Error& e) {
        return e.kind == Fail::Input;
    } catch (...) {
        return false;
    }
    return false;
}

u32 get32(const std::vector<uint8_t>& b, size_t p) {
    return u32(b[p]) | u32(b[p + 1]) << 8 | u32(b[p + 2]) << 16 | u32(b[p + 3]) << 24;
}
u64 get64(const std::vector<uint8_t>& b, size_t p) { return u64(get32(b, p)) | u64(get32(b, p + 4)) << 32; }
void put32(std::vector<uint8_t>& b, size_t pos, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[pos + i] = uint8_t(v >> (8 * i));
}
void put64(std::vector<uint8_t>& b, size_t pos, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[pos + i] = uint8_t(v >> (8 * i));
}

// Absolute image offset of the folder (dir=true) or file entry with this name, found by walking the packed table the way the
// builder laid it out. Returns kNotFound if absent.
u64 find_entry(const std::vector<uint8_t>& img, bool dir, const std::string& name) {
    const u64 table = get64(img, dir ? 0x18 : 0x38);
    const u64 size = get64(img, dir ? 0x20 : 0x40);
    const u64 hdr = dir ? 0x18 : 0x20;
    const size_t name_len_at = dir ? 0x14 : 0x1C;
    for (u64 o = 0; o + hdr <= size;) {
        const u32 n = get32(img, size_t(table + o + name_len_at));
        if (n == name.size() && std::equal(name.begin(), name.end(), img.begin() + std::ptrdiff_t(table + o + hdr)))
            return table + o;
        o += (hdr + n + 3) & ~u64(3);
    }
    return kNotFound;
}

SynthFiles small_tree() {
    return {
        {"root.bin", pattern(5, 1)},
        {"dir1/a.txt", pattern(7, 2)},
        {"dir1/sub/deep/c.bin", pattern(9, 3)},
        {"zero.bin", {}},
        {"dir2/caf\xc3\xa9.bin", pattern(4, 4)},
        {"dir2/with space/name here.txt", pattern(3, 5)},
        {"dir1/sub/top.bin", pattern(6, 6)},
    };
}

void check_small_tree() {
    const SynthFiles in = small_tree();
    const std::vector<uint8_t> img = synth_romfs(in);
    auto r = Romfs::open(mem(img));
    CHECK(r->image() != nullptr);

    // Files: a folder's files first, then its subfolders, depth first.
    const std::vector<std::string> want_files = {
        "root.bin",
        "zero.bin",
        "dir1/a.txt",
        "dir1/sub/top.bin",
        "dir1/sub/deep/c.bin",
        "dir2/caf\xc3\xa9.bin",
        "dir2/with space/name here.txt",
    };
    const std::vector<std::string> want_dirs = {
        "dir1",
        "dir1/sub",
        "dir1/sub/deep",
        "dir2",
        "dir2/with space",
    };
    CHECK_EQ(r->files().size(), want_files.size());
    for (size_t i = 0; i < std::min(r->files().size(), want_files.size()); ++i)
        CHECK(r->files()[i].path == want_files[i]);
    CHECK(r->dirs() == want_dirs);

    uint64_t total = 0;
    for (const auto& f : in) total += f.second.size();
    CHECK_EQ(r->total_file_bytes(), total);

    // Each file's bytes come back exactly, matched by path.
    for (const RomfsFile& f : r->files()) {
        bool matched = false;
        for (const auto& in_f : in) {
            if (in_f.first != f.path) continue;
            matched = true;
            CHECK_EQ(f.size, uint64_t(in_f.second.size()));
            CHECK(read_all(*r->open_file(f), 0, size_t(f.size)) == in_f.second);
        }
        CHECK(matched);
    }
}

void check_big_dir() {
    SynthFiles in;
    uint64_t total = 0;
    for (int i = 0; i < 200; ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "big/f%03d.bin", i);
        in.push_back({name, pattern(size_t(i % 50), uint8_t(i))});   // includes 0-byte files
        total += i % 50;
    }
    const std::vector<uint8_t> img = synth_romfs(in);
    auto r = Romfs::open(mem(img));
    CHECK_EQ(r->files().size(), size_t(200));
    CHECK(r->dirs() == std::vector<std::string>{"big"});
    CHECK_EQ(r->total_file_bytes(), total);
    for (size_t i = 0; i < std::min<size_t>(r->files().size(), in.size()); ++i) {
        CHECK(r->files()[i].path == in[i].first);
        CHECK(read_all(*r->open_file(r->files()[i]), 0, size_t(r->files()[i].size)) == in[i].second);
    }
}

// A chain of `depth` folders with one file at the bottom.
SynthFiles nested(size_t depth) {
    std::string p;
    for (size_t i = 0; i < depth; ++i) p += "d/";
    p += "f.bin";
    return {{p, pattern(3, 9)}};
}

void check_depth() {
    {
        auto r = Romfs::open(mem(synth_romfs(nested(256))));   // 256 is allowed
        CHECK_EQ(r->dirs().size(), size_t(256));
        CHECK_EQ(r->files().size(), size_t(1));
    }
    CHECK(throws_input([&] { (void)Romfs::open(mem(synth_romfs(nested(300)))); }));
    CHECK(throws_input([&] { (void)Romfs::open(mem(synth_romfs(nested(257)))); }));
}

void check_corruptions() {
    const std::vector<uint8_t> base = synth_romfs(small_tree());
    const u64 root_bin = find_entry(base, false, "root.bin");
    const u64 a_txt = find_entry(base, false, "a.txt");
    const u64 deep = find_entry(base, true, "deep");
    const u64 dir1 = find_entry(base, true, "dir1");
    CHECK(root_bin != kNotFound && a_txt != kNotFound && deep != kNotFound && dir1 != kNotFound);
    if (root_bin == kNotFound || a_txt == kNotFound || deep == kNotFound || dir1 == kNotFound) return;

    auto expect_fail = [](const std::vector<uint8_t>& img) {
        return throws_input([&] { (void)Romfs::open(mem(img)); });
    };

    {
        auto b = base;
        put64(b, 0x00, 0x58);             // header_size
        CHECK(expect_fail(b));
    }
    {
        auto b = base;
        put64(b, 0x18, 1ULL << 40);       // folder list table offset past the image
        CHECK(expect_fail(b));
    }
    {
        auto b = base;
        put64(b, 0x48, 1ULL << 40);       // file_data_offset past the image
        CHECK(expect_fail(b));
    }
    {
        // Child-folder pointer of "deep" set back to "dir1", its own ancestor: a real cycle.
        auto b = base;
        put32(b, size_t(deep) + 0x08, uint32_t(dir1 - get64(base, 0x18)));
        CHECK(expect_fail(b));
    }
    {
        // "root.bin" is its own next sibling: a file loop.
        auto b = base;
        put32(b, size_t(root_bin) + 0x04, uint32_t(root_bin - get64(base, 0x38)));
        CHECK(expect_fail(b));
    }
    {
        auto b = base;
        put64(b, size_t(root_bin) + 0x10, 1ULL << 40);   // data_size huge
        CHECK(expect_fail(b));
    }
    {
        auto b = base;
        put32(b, size_t(root_bin) + 0x04, 0xFFFFFF00);   // sibling offset outside the file table
        CHECK(expect_fail(b));
    }
    {
        auto b = base;
        b[size_t(a_txt) + 0x20 + 1] = '/';               // "a.txt" -> "a/txt"
        CHECK(expect_fail(b));
    }
    {
        auto b = base;
        put32(b, size_t(a_txt) + 0x1C, 0);              // empty name
        CHECK(expect_fail(b));
    }
    {
        auto b = base;
        b.resize(0x30);                                   // smaller than the header
        CHECK(expect_fail(b));
    }
    // Name ".." and "." as a root-level file.
    CHECK(expect_fail(synth_romfs({{"..", pattern(3, 1)}})));
    CHECK(expect_fail(synth_romfs({{".", pattern(3, 1)}})));
}

}  // namespace

int main() {
    check_small_tree();
    check_big_dir();
    check_depth();
    check_corruptions();
    return TEST_SUMMARY("test_romfs");
}
