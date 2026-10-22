// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the PFS0 / HFS0 parser (src/pfs.cpp) on synthetic containers from synth.cpp.
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "common.h"
#include "pfs.h"
#include "synth.h"

using namespace mprs;
using mprs_test::SynthFiles;
using mprs_test::synth_hfs0;
using mprs_test::synth_pfs0;

namespace {

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 7 + (i >> 8));
    return v;
}

StoragePtr mem(std::vector<uint8_t> b) { return make_memory_storage(std::move(b)); }

// True when fn throws mprs::Error with kind Input. Any other exception, or no exception, is false.
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

void put32(std::vector<uint8_t>& b, size_t pos, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[pos + i] = uint8_t(v >> (8 * i));
}
void put64(std::vector<uint8_t>& b, size_t pos, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[pos + i] = uint8_t(v >> (8 * i));
}

// 0 files, 1 file, or 5 files (one empty, one 100 KiB). The last file is never empty, so trimming the end always breaks it.
SynthFiles make_files(int count) {
    if (count == 0) return {};
    if (count == 1) return {{"only.bin", pattern(10, 1)}};
    return {
        {"a.bin", {}},
        {"b.bin", pattern(100, 2)},
        {"c.bin", pattern(100 * 1024, 3)},
        {"d.bin", pattern(1, 4)},
        {"e.bin", pattern(37, 5)},
    };
}

size_t data_bytes(const SynthFiles& files) {
    size_t n = 0;
    for (const auto& f : files) n += f.second.size();
    return n;
}

void round_trip_pfs0(const SynthFiles& files) {
    const std::vector<uint8_t> bytes = synth_pfs0(files);
    auto p = Pfs::open(mem(bytes));
    CHECK(p->kind() == Pfs::Kind::Pfs0);
    CHECK_EQ(p->entries().size(), files.size());

    uint64_t strtab = 0;
    for (const auto& f : files) strtab += f.first.size() + 1;
    CHECK_EQ(p->header_size(), uint64_t(0x10 + 0x18 * files.size() + strtab));
    CHECK_EQ(uint64_t(bytes.size()), p->header_size() + data_bytes(files));

    uint64_t rel = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const PfsEntry& e = p->entries()[i];
        CHECK(e.name == files[i].first);
        CHECK_EQ(e.size, uint64_t(files[i].second.size()));
        CHECK_EQ(e.offset, p->header_size() + rel);
        CHECK_EQ(e.hashed_size, 0u);
        CHECK(std::all_of(e.sha256.begin(), e.sha256.end(), [](uint8_t b) { return b == 0; }));
        CHECK(p->find(e.name) != nullptr);
        CHECK(read_all(*p->open_file(e), 0, size_t(e.size)) == files[i].second);
        rel += files[i].second.size();
    }
    CHECK(p->find("no-such-file.bin") == nullptr);
}

void round_trip_hfs0(const SynthFiles& files) {
    const std::vector<uint8_t> bytes = synth_hfs0(files);
    auto p = Pfs::open(mem(bytes));
    CHECK(p->kind() == Pfs::Kind::Hfs0);
    CHECK_EQ(p->entries().size(), files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        const PfsEntry& e = p->entries()[i];
        const auto& data = files[i].second;
        const uint32_t hashed = uint32_t(std::min<size_t>(data.size(), 0x200));
        CHECK(e.name == files[i].first);
        CHECK_EQ(e.hashed_size, hashed);
        CHECK(e.sha256 == mprs_test::synth_sha256(data.data(), hashed));
        CHECK(read_all(*p->open_file(e), 0, size_t(e.size)) == data);
        CHECK(p->find(e.name) != nullptr);
    }
}

}  // namespace

int main() {
    // Round trips: 0, 1 and 5 files.
    round_trip_pfs0(make_files(0));
    round_trip_pfs0(make_files(1));
    round_trip_pfs0(make_files(5));
    round_trip_hfs0(make_files(0));
    round_trip_hfs0(make_files(1));
    round_trip_hfs0(make_files(5));

    // find() hit and miss on the five-file set, and entry offsets are absolute.
    {
        auto p = Pfs::open(mem(synth_pfs0(make_files(5))));
        const PfsEntry* hit = p->find("c.bin");
        CHECK(hit != nullptr);
        if (hit) CHECK_EQ(hit->size, uint64_t(100 * 1024));
        CHECK(p->find("c") == nullptr);
        CHECK(p->find("C.bin") == nullptr);
        CHECK_EQ(p->entries()[0].offset, p->header_size());
    }

    // SHA-256 check against published test vectors (independent of the parser).
    {
        const SynthFiles abc = {{"abc", {'a', 'b', 'c'}}};
        auto p = Pfs::open(mem(synth_hfs0(abc)));
        CHECK_EQ(hex_encode(p->entries()[0].sha256.data(), 32),
                 std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        const SynthFiles empty = {{"empty", {}}};
        auto q = Pfs::open(mem(synth_hfs0(empty)));
        CHECK_EQ(q->entries()[0].hashed_size, 0u);
        CHECK_EQ(hex_encode(q->entries()[0].sha256.data(), 32),
                 std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    }

    // Extra zero bytes in the string table move the data start.
    {
        const SynthFiles files = make_files(5);
        const std::vector<uint8_t> bytes = synth_pfs0(files, 7);
        auto p = Pfs::open(mem(bytes));
        uint64_t strtab = 7;
        for (const auto& f : files) strtab += f.first.size() + 1;
        CHECK_EQ(p->header_size(), uint64_t(0x10 + 0x18 * files.size() + strtab));
        CHECK(read_all(*p->open_file(p->entries()[4]), 0, 37) == files[4].second);
    }

    // Negative cases on a valid five-file PFS0 (last file non-empty).
    const std::vector<uint8_t> base = synth_pfs0(make_files(5));

    {
        auto b = base;
        b.pop_back();
        CHECK(throws_input([&] { (void)Pfs::open(mem(b)); }));
    }
    {
        auto b = base;
        b[0] = 'X';
        CHECK(throws_input([&] { (void)Pfs::open(mem(b)); }));
    }
    {
        auto b = base;
        put64(b, 0x10 + 8, 1ULL << 40);   // entry 0 size
        CHECK(throws_input([&] { (void)Pfs::open(mem(b)); }));
    }
    {
        auto b = base;
        put64(b, 0x10, 1ULL << 40);       // entry 0 offset
        CHECK(throws_input([&] { (void)Pfs::open(mem(b)); }));
    }
    {
        auto b = base;
        put32(b, 0x10 + 16, 0x00FFFFFF);  // entry 0 name offset past the table
        CHECK(throws_input([&] { (void)Pfs::open(mem(b)); }));
    }
    {
        auto b = base;
        b.resize(0x10 + 10);              // cut inside the entry table
        CHECK(throws_input([&] { (void)Pfs::open(mem(b)); }));
    }
    {
        auto b = base;
        b.resize(8);                      // smaller than the header
        CHECK(throws_input([&] { (void)Pfs::open(mem(b)); }));
    }
    {
        auto b = base;
        put32(b, 4, 0x7fffffff);          // file_count
        CHECK(throws_input([&] { (void)Pfs::open(mem(b)); }));
    }
    {
        // Name not terminated inside the table: "abc\0" becomes "abcz" as the last table bytes.
        auto b = synth_pfs0({{"abc", pattern(4, 1)}});
        b[0x10 + 0x18 + 3] = 'z';
        CHECK(throws_input([&] { (void)Pfs::open(mem(b)); }));
    }
    // Bad names, each as the only file.
    for (const char* bad : {"../x", "a/b", "a\\b", "x:y", ".", "..", ""}) {
        auto b = synth_pfs0({{bad, pattern(4, 1)}});
        CHECK(throws_input([&] { (void)Pfs::open(mem(b)); }));
    }
    // HFS0 negatives.
    {
        auto h = synth_hfs0(make_files(5));
        h.pop_back();
        CHECK(throws_input([&] { (void)Pfs::open(mem(h)); }));
    }
    {
        auto h = synth_hfs0(make_files(5));
        put64(h, 0x10 + 8, 1ULL << 40);
        CHECK(throws_input([&] { (void)Pfs::open(mem(h)); }));
    }
    {
        auto h = synth_hfs0(make_files(5));
        h[1] = 'X';
        CHECK(throws_input([&] { (void)Pfs::open(mem(h)); }));
    }

    return TEST_SUMMARY("test_pfs");
}
