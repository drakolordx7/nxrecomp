// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the NCA reader (src/nca.cpp) on synthetic NCAs from synth_nca.cpp: header decryption, key selection, section
// decryption (key area and ticket paths), PFS0 / RomFS access, hash-tree verification, truncation and magic cases.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "common.h"
#include "keys.h"
#include "nca.h"
#include "nso.h"
#include "pfs.h"
#include "romfs.h"
#include "storage.h"
#include "synth.h"
#include "synth_nca.h"

using namespace mprs;
using mprs_test::Bytes16;
using mprs_test::SynthCorruption;
using mprs_test::SynthFiles;
using mprs_test::SynthKeys;
using mprs_test::SynthNcaSpec;
using mprs_test::SynthSection;

namespace {

constexpr uint64_t kProgramId = 0x010012101468C000ull;
const char* const kBuildId = "0123456789ABCDEF0123456789ABCDEF01234567";

struct Outcome {
    bool threw = false;
    Fail kind = Fail::Generic;
    std::string msg;
};

template <class F>
Outcome run(F&& f) {
    Outcome o;
    try {
        f();
    } catch (const Error& e) {
        o.threw = true;
        o.kind = e.kind;
        o.msg = e.what();
    } catch (...) {
        o.threw = true;
        o.msg = "non-Error exception";
    }
    return o;
}

std::string tmp_dir(const char* name) {
    const auto p = std::filesystem::temp_directory_path() / (std::string("mprs_test_nca_") + name);
    std::filesystem::create_directories(p);
    return p.string();
}

StoragePtr mem(std::vector<uint8_t> b) { return make_memory_storage(std::move(b)); }

KeySet load_keys(const std::string& path) {
    KeySet k;
    k.load_file(path);
    return k;
}

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 7 + (i >> 8));
    return v;
}

std::string hex16(const Bytes16& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t x : b) {
        s.push_back(d[x >> 4]);
        s.push_back(d[x & 15]);
    }
    return s;
}

SynthFiles romfs_files() {
    return {
        {"Worlds/MP1/Test.pak", pattern(5000, 1)},
        {"Audio/ding.bin", pattern(20000, 2)},
        {"readme.txt", std::vector<uint8_t>{'h', 'e', 'l', 'l', 'o'}},
    };
}

SynthNcaSpec program_spec(const SynthFiles& romfs) {
    SynthNcaSpec sp;
    sp.content_type = 0;
    sp.program_id = kProgramId;
    sp.content_index = 0;
    sp.key_generation = 11;
    sp.sections = {SynthSection{1, 3, mprs_test::synth_exefs(kBuildId)}, SynthSection{0, 3, mprs_test::synth_romfs(romfs)}};
    return sp;
}

// Reads `len` bytes at `off` from the storage and compares them with `expect[off, off+len)`.
bool same_range(const IStorage& s, const std::vector<uint8_t>& expect, uint64_t off, size_t len) {
    if (off + len > expect.size() || s.size() != expect.size()) return false;
    std::vector<uint8_t> got(len);
    s.read(off, got.data(), len);
    return std::equal(got.begin(), got.end(), expect.begin() + static_cast<std::ptrdiff_t>(off));
}

void check_round_trip(const Nca& nca, int index, const std::vector<uint8_t>& plain) {
    auto st = nca.open_section(index);
    CHECK_EQ(st->size(), plain.size());
    CHECK(same_range(*st, plain, 0, plain.size()));
}

}  // namespace

int main() {
    const std::string dir = tmp_dir("nca");
    const std::string keys_path = mprs_test::write_synth_keys_file(dir);
    const KeySet keys = load_keys(keys_path);
    const SynthFiles files = romfs_files();
    const std::vector<uint8_t> exefs = mprs_test::synth_exefs(kBuildId);
    const std::vector<uint8_t> romfs = mprs_test::synth_romfs(files);

    // ---- the independent AES used by the builder: FIPS-197 C.1 (AES-128) ----
    {
        uint8_t key[16], blk[16];
        for (int i = 0; i < 16; ++i) {
            key[i] = uint8_t(i);
            blk[i] = uint8_t((i << 4) | i);
        }
        mprs_test::synth_aes128_encrypt_block(key, blk);
        CHECK_EQ(hex_encode(blk, 16), std::string("69c4e0d86a7b0430d8cdb78070b4c55a"));
    }

    // Preconditions the tamper tests rely on: the last block of each image is a partial one.
    CHECK(exefs.size() % 0x1000 != 0);
    CHECK(romfs.size() % 0x4000 != 0);

    // ---- header decryption ----
    const SynthNcaSpec base = program_spec(files);
    const mprs_test::SynthNcaBuild built = mprs_test::synth_nca_build(base);
    {
        auto nca = Nca::open(mem(built.bytes), keys, TicketKeys{});
        CHECK(nca->content_type() == NcaContentType::Program);
        CHECK_EQ(nca->program_id(), kProgramId);
        CHECK_EQ(nca->content_index(), 0u);
        CHECK_EQ(nca->content_size(), uint64_t(built.bytes.size()));
        CHECK_EQ(nca->key_generation(), 10);
        CHECK(!nca->has_rights_id());
        CHECK(nca->section(0) != nullptr);
        CHECK(nca->section(1) != nullptr);
        CHECK(nca->section(2) == nullptr);
        if (nca->section(0)) {
            CHECK_EQ(nca->section(0)->offset, uint64_t(0xC00));
            CHECK_EQ(nca->section(0)->fs_type, 1);
            CHECK_EQ(nca->section(0)->hash_type, 2);
            CHECK_EQ(nca->section(0)->enc_type, 3);
        }
        if (nca->section(1)) {
            CHECK_EQ(nca->section(1)->fs_type, 0);
            CHECK_EQ(nca->section(1)->hash_type, 3);
            CHECK_EQ(nca->section(1)->enc_type, 3);
        }
    }
    {
        // A meta NCA (content type 1) with version-0 CNMT built by the builder.
        auto meta = Nca::open(mem(mprs_test::synth_cnmt_nca(kProgramId, 0)), keys, TicketKeys{});
        CHECK(meta->content_type() == NcaContentType::Meta);
        CHECK_EQ(meta->program_id(), kProgramId);
    }

    // ---- wrong keys ----
    {
        SynthKeys bad = SynthKeys::standard();
        bad.header_kek_source[0] ^= 0xFF;
        const KeySet bad_keys = load_keys(mprs_test::write_synth_keys_file_with(dir + "/bad", bad, "prod.keys"));
        const Outcome o = run([&] { Nca::open(mem(built.bytes), bad_keys, TicketKeys{}); });
        CHECK(o.threw && o.kind == Fail::Keys);
        if (!(o.threw && o.kind == Fail::Keys)) std::printf("  got: %s\n", o.msg.c_str());
    }

    // ---- key area section decrypt round trip ----
    {
        auto nca = Nca::open(mem(built.bytes), keys, TicketKeys{});
        check_round_trip(*nca, 0, built.plain[0]);
        check_round_trip(*nca, 1, built.plain[1]);
        // Unaligned reads across the section equal the plain bytes (CTR starts at a 16-byte boundary, so the offset matters).
        auto st = nca->open_section(1);
        const uint64_t n = built.plain[1].size();
        const std::pair<uint64_t, size_t> cases[] = {{1, 1}, {15, 17}, {16, 16}, {0x1FF, 3}, {0x3FF0, 0x40}, {n - 5, 5}, {3, size_t(n - 7)}};
        for (const auto& c : cases) {
            CHECK(same_range(*st, built.plain[1], c.first, c.second));
        }
        auto st0 = nca->open_section(0);
        const uint64_t n0 = built.plain[0].size();
        CHECK(same_range(*st0, built.plain[0], 7, size_t(n0 - 9)));
        CHECK(same_range(*st0, built.plain[0], n0 / 3, size_t(n0 / 2)));
    }

    // ---- ExeFS (PFS0) listing and the NSO build id ----
    {
        auto nca = Nca::open(mem(built.bytes), keys, TicketKeys{});
        auto p = nca->open_pfs0(0);
        std::vector<std::string> names;
        for (const PfsEntry& e : p->entries()) names.push_back(e.name);
        const std::vector<std::string> want = {"main", "rtld", "sdk", "subsdk0", "main.npdm"};
        CHECK(names == want);
        auto ref = Pfs::open(mem(exefs));
        bool all_equal = ref->entries().size() == p->entries().size();
        for (const PfsEntry& e : ref->entries()) {
            const PfsEntry* got = p->find(e.name);
            if (!got) { all_equal = false; continue; }
            if (read_all(*p->open_file(*got), 0, size_t(got->size)) != read_all(*ref->open_file(e), 0, size_t(e.size)))
                all_equal = false;
        }
        CHECK(all_equal);
        if (const PfsEntry* m = p->find("main")) {
            const NsoInfo info = nso_read_info(*p->open_file(*m));
            CHECK(info.build_id() == kBuildId);
        } else {
            CHECK(false);
        }
    }

    // ---- RomFS image ----
    {
        auto nca = Nca::open(mem(built.bytes), keys, TicketKeys{});
        auto img = nca->open_romfs_image(1);
        CHECK_EQ(img->size(), uint64_t(romfs.size()));
        CHECK(read_all(*img, 0, size_t(img->size())) == romfs);
        auto rf = Romfs::open(img);
        CHECK_EQ(rf->files().size(), files.size());
        for (const auto& want : files) {
            bool found = false;
            for (const RomfsFile& f : rf->files()) {
                if (f.path != want.first) continue;
                found = true;
                CHECK(read_all(*rf->open_file(f), 0, size_t(f.size)) == want.second);
            }
            CHECK(found);
        }
    }

    // ---- hash-tree verification: clean sections pass ----
    {
        auto nca = Nca::open(mem(built.bytes), keys, TicketKeys{});
        CHECK(nca->verify_section(0, nullptr, nullptr) >= exefs.size());
        CHECK(nca->verify_section(1, nullptr, nullptr) >= romfs.size());
    }

    // ---- hash-tree verification: each tamper is caught (Fail::Input) ----
    using W = SynthCorruption::Where;
    const std::pair<int, W> tampers[] = {{0, W::HashLayer}, {0, W::DataFirstByte}, {0, W::DataLastByte}, {0, W::MasterHash},
                                         {1, W::HashLayer}, {1, W::DataFirstByte}, {1, W::DataLastByte}, {1, W::MasterHash}};
    for (const auto& t : tampers) {
        SynthNcaSpec sp = base;
        sp.corrupt = {SynthCorruption{t.first, t.second}};
        const mprs_test::SynthNcaBuild b = mprs_test::synth_nca_build(sp);
        auto nca = Nca::open(mem(b.bytes), keys, TicketKeys{});   // the header itself is intact
        const Outcome o = run([&] { nca->verify_section(t.first, nullptr, nullptr); });
        CHECK(o.threw && o.kind == Fail::Input);
        if (!(o.threw && o.kind == Fail::Input)) std::printf("  section %d tamper %d: %s\n", t.first, int(t.second), o.msg.c_str());
    }

    // ---- truncation ----
    {
        SynthNcaSpec sp = base;
        sp.truncate = true;
        sp.truncate_to = built.bytes.size() - 0x200;   // the last section ends past the end
        const mprs_test::SynthNcaBuild b = mprs_test::synth_nca_build(sp);
        const Outcome o = run([&] { Nca::open(mem(b.bytes), keys, TicketKeys{}); });
        CHECK(o.threw && o.kind == Fail::Input);
        sp.truncate_to = 0xBFF;                         // shorter than the header
        const Outcome o2 = run([&] { Nca::open(mem(mprs_test::synth_nca(sp)), keys, TicketKeys{}); });
        CHECK(o2.threw && o2.kind == Fail::Input);
    }

    // ---- ticket / rights id paths ----
    {
        const Bytes16 rights = mprs_test::synth_dump_rights_id();
        const Bytes16 titlekey = mprs_test::synth_dump_titlekey();
        SynthNcaSpec rs = base;
        rs.has_rights = true;
        rs.rights_id = rights;
        rs.titlekey = titlekey;
        const mprs_test::SynthNcaBuild rb = mprs_test::synth_nca_build(rs);

        // With the title key from a ticket (common), the section decrypts.
        TicketKeys tk;
        tk.common[hex16(rights)] = mprs_test::synth_encrypt_titlekey(11, titlekey);
        auto nca = Nca::open(mem(rb.bytes), keys, tk);
        CHECK(nca->has_rights_id());
        CHECK(nca->rights_id() == rights);
        check_round_trip(*nca, 0, rb.plain[0]);
        check_round_trip(*nca, 1, rb.plain[1]);

        // No ticket, no title.keys entry: Fail::Keys when the section is opened.
        auto no_ticket = Nca::open(mem(rb.bytes), keys, TicketKeys{});
        const Outcome o = run([&] { no_ticket->open_section(0); });
        CHECK(o.threw && o.kind == Fail::Keys);

        // Personalized ticket: Fail::Keys, and the message names the console.
        TicketKeys ptk;
        ptk.personalized.push_back(hex16(rights));
        auto pers = Nca::open(mem(rb.bytes), keys, ptk);
        const Outcome po = run([&] { pers->open_section(0); });
        CHECK(po.threw && po.kind == Fail::Keys);
        CHECK(po.msg.find("console") != std::string::npos);

        // title.keys line in the keys file ("rights id = encrypted title key") works without a ticket.
        const KeySet tkeys = load_keys(mprs_test::write_synth_keys_file_with(
            dir + "/titlekeys", SynthKeys::standard(), "prod.keys",
            {{rights, mprs_test::synth_encrypt_titlekey(11, titlekey)}}));
        auto via_file = Nca::open(mem(rb.bytes), tkeys, TicketKeys{});
        check_round_trip(*via_file, 1, rb.plain[1]);
    }

    // ---- NCA2 (per-section header encryption) and NCA0 (rejected) ----
    {
        SynthNcaSpec s2 = base;
        s2.magic_digit = '2';
        const mprs_test::SynthNcaBuild b2 = mprs_test::synth_nca_build(s2);
        auto nca = Nca::open(mem(b2.bytes), keys, TicketKeys{});
        CHECK(nca->section(0) != nullptr && nca->section(1) != nullptr);
        check_round_trip(*nca, 0, b2.plain[0]);
        CHECK(nca->verify_section(1, nullptr, nullptr) >= romfs.size());

        SynthNcaSpec s0 = base;
        s0.magic_digit = '0';
        const Outcome o = run([&] { Nca::open(mem(mprs_test::synth_nca(s0)), keys, TicketKeys{}); });
        CHECK(o.threw && o.kind == Fail::Input);
    }

    return TEST_SUMMARY("test_nca");
}
