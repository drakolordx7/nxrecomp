// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for updates and DLC: ticket title keys, CNMT classification across several inputs, the patch RomFS (IndirectStorage over the
// base RomFS + AES-CTR-EX) built by synth_nca.cpp, and a whole run_setup with base + update + DLC.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "bktr.h"
#include "check.h"
#include "common.h"
#include "container.h"
#include "keys.h"
#include "log.h"
#include "nca.h"
#include "nso.h"
#include "pipeline.h"
#include "romfs.h"
#include "synth.h"
#include "synth_nca.h"

using namespace mprs;
using mprs_test::Bytes16;
using mprs_test::SynthFiles;
using mprs_test::SynthNcaSpec;
using mprs_test::SynthSection;
namespace fs = std::filesystem;

namespace {

constexpr u64 kApp = 0x010012101468C000ull;
const char* const kTitleId = "010012101468C000";
const char* const kBaseBuild = "0123456789ABCDEF0123456789ABCDEF01234567";
const char* const kOldBuild = "1111111111111111111111111111111111111111";
const char* const kNewBuild = "FEDCBA9876543210FEDCBA987654321000000000";

std::string tmp_dir(const char* name) {
    const fs::path p = fs::temp_directory_path() / (std::string("mprs_test_update_") + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p.string();
}

void write_file(const std::string& path, const std::vector<uint8_t>& d) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 7 + (i >> 8));
    return v;
}

void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }

std::string tid_hex(u64 t) {
    char b[20];
    std::snprintf(b, sizeof b, "%016llX", (unsigned long long)t);
    return b;
}

// A content id (16 bytes) from a seed, as 32 lower-case hex digits.
std::string cid(int seed) {
    uint8_t b[16];
    for (int i = 0; i < 16; ++i) b[i] = uint8_t(seed * 31 + i * 17 + 1);
    return hex_encode(b, 16);
}

struct Rec { std::string id; uint8_t type; };

// A .cnmt with an extended header (application id for Patch / AddOnContent) and content records.
std::vector<uint8_t> cnmt_bytes(u64 tid, u32 version, uint8_t type, u64 app_id, const std::vector<Rec>& recs) {
    const size_t ext = 0x10;
    std::vector<uint8_t> c(0x20 + ext + 0x38 * recs.size(), 0);
    put64(c.data(), tid);
    put32(c.data() + 8, version);
    c[0xC] = type;
    c[0xE] = uint8_t(ext);
    c[0x10] = uint8_t(recs.size());
    put64(c.data() + 0x20, app_id);
    for (size_t i = 0; i < recs.size(); ++i) {
        uint8_t* r = c.data() + 0x20 + ext + 0x38 * i;
        hex_decode(recs[i].id, r + 0x20, 16);
        r[0x30] = 0x10;                                     // size (not checked)
        r[0x36] = recs[i].type;
    }
    return c;
}

std::vector<uint8_t> meta_nca(u64 tid, const std::vector<uint8_t>& cnmt) {
    SynthNcaSpec sp;
    sp.content_type = 1;
    sp.program_id = tid;
    sp.sections = {SynthSection{1, 3, mprs_test::synth_pfs0({{"Meta_" + tid_hex(tid) + ".cnmt", cnmt}})}};
    return mprs_test::synth_nca(sp);
}

KeySet load_keys(const std::string& path) {
    KeySet k;
    k.load_file(path);
    return k;
}

template <class F>
std::string error_of(F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

// ---- the synthetic game: base 1.0, update v1 (in the XCI) and v2 (NSP, with a ticket), two DLC ----
const SynthFiles kBaseFiles = {{"data/a.bin", pattern(40000, 1)}, {"data/b.bin", pattern(9000, 2)}, {"readme.txt", {'v', '1'}}};
// The update changes b.bin, adds c.bin (large, so its AES-CTR-EX table needs two entry sets) and keeps a.bin.
const SynthFiles kNewFiles = {{"data/a.bin", pattern(40000, 1)},
                              {"data/b.bin", pattern(9000, 99)},
                              {"data/c.bin", pattern(90000, 5)},
                              {"readme.txt", {'v', '2'}}};
const SynthFiles kOldFiles = {{"data/a.bin", pattern(40000, 1)}, {"data/b.bin", pattern(9000, 50)}, {"readme.txt", {'v', '0'}}};
const SynthFiles kDlcA = {{"aoc/item.bin", pattern(3000, 7)}};
const SynthFiles kDlcB = {{"more.bin", pattern(500, 8)}};

Bytes16 seeded(int s) {
    Bytes16 b;
    for (int i = 0; i < 16; ++i) b[i] = uint8_t(s * 13 + i * 29 + 3);
    return b;
}

struct Game {
    std::string dir, keys, xci, nsp, base_only;
    std::vector<uint8_t> new_section;   // the patched RomFS section as the reader must see it
};

std::vector<uint8_t> dlc_nca(u64 tid, const SynthFiles& files) {
    SynthNcaSpec sp;
    sp.content_type = 5;   // PublicData
    sp.program_id = tid;
    sp.sections = {SynthSection{0, 3, mprs_test::synth_romfs(files)}};
    return mprs_test::synth_nca(sp);
}

Game make_game(const char* name, bool damage_patch = false) {
    Game g;
    g.dir = tmp_dir(name);
    g.keys = mprs_test::write_synth_keys_file(g.dir);

    SynthNcaSpec base;
    base.program_id = kApp;
    base.sections = {SynthSection{1, 3, mprs_test::synth_exefs(kBaseBuild)}, SynthSection{0, 3, mprs_test::synth_romfs(kBaseFiles)}};
    const mprs_test::SynthNcaBuild bb = mprs_test::synth_nca_build(base);

    auto patch_nca = [&](const char* build, const SynthFiles& files, bool rights, mprs_test::SynthNcaBuild* out) {
        SynthNcaSpec sp;
        sp.program_id = kApp;   // an update's program carries the game's own id
        SynthSection rs{0, 4, mprs_test::synth_romfs(files)};
        rs.patch_base = bb.plain[1];
        sp.sections = {SynthSection{1, 3, mprs_test::synth_exefs(build)}, rs};
        if (rights) {
            sp.has_rights = true;
            sp.rights_id = seeded(40);
            sp.titlekey = seeded(41);
        }
        *out = mprs_test::synth_nca_build(sp);
        return out->bytes;
    };
    mprs_test::SynthNcaBuild old_b, new_b;
    const std::vector<uint8_t> old_patch = patch_nca(kOldBuild, kOldFiles, false, &old_b);
    std::vector<uint8_t> new_patch = patch_nca(kNewBuild, kNewFiles, true, &new_b);
    g.new_section = new_b.plain[1];
    if (damage_patch) new_patch[0xC00 + new_b.plain[0].size() + 0x10] ^= 0x40;   // the first bytes of the update's own data

    const u64 patch_id = kApp + 0x800, dlc_a = kApp + 0x1001, dlc_b = kApp + 0x1002;
    const SynthFiles xci_entries = {
        {cid(1) + ".nca", bb.bytes},
        {cid(2) + ".cnmt.nca", meta_nca(kApp, cnmt_bytes(kApp, 0, 0x80, patch_id, {{cid(1), 1}}))},
        {cid(3) + ".nca", old_patch},
        {cid(4) + ".cnmt.nca", meta_nca(patch_id, cnmt_bytes(patch_id, 0x10000, 0x81, kApp, {{cid(3), 1}}))},
        {cid(5) + ".nca", dlc_nca(dlc_a, kDlcA)},
        {cid(6) + ".cnmt.nca", meta_nca(dlc_a, cnmt_bytes(dlc_a, 0, 0x82, kApp, {{cid(5), 2}}))},
    };
    g.xci = g.dir + "/game.xci";
    write_file(g.xci, mprs_test::synth_xci(xci_entries));

    const SynthFiles nsp_entries = {
        {cid(7) + ".nca", new_patch},
        {cid(8) + ".cnmt.nca", meta_nca(patch_id, cnmt_bytes(patch_id, 0x20000, 0x81, kApp, {{cid(7), 1}}))},
        {hex_encode(seeded(40).data(), 16) + ".tik",
         mprs_test::synth_ticket(seeded(40), mprs_test::synth_encrypt_titlekey(11, seeded(41)), false)},
        {cid(5) + ".nca", dlc_nca(dlc_a, kDlcA)},   // the same DLC again
        {cid(6) + ".cnmt.nca", meta_nca(dlc_a, cnmt_bytes(dlc_a, 0, 0x82, kApp, {{cid(5), 2}}))},
        {cid(9) + ".nca", dlc_nca(dlc_b, kDlcB)},
        {cid(10) + ".cnmt.nca", meta_nca(dlc_b, cnmt_bytes(dlc_b, 0, 0x82, kApp, {{cid(9), 2}}))},
    };
    g.nsp = g.dir + "/update_dlc.nsp";
    write_file(g.nsp, mprs_test::synth_nsp(nsp_entries));

    g.base_only = g.dir + "/base.nsp";
    write_file(g.base_only, mprs_test::synth_nsp({xci_entries[0], xci_entries[1]}));
    return g;
}

// ---- tests ------------------------------------------------------------------------------------------------------------------

void test_ticket_titlekey(const std::string& root) {
    // The program NCA is encrypted with a title key; the NSP's common ticket holds it encrypted with titlekek[key generation].
    const Bytes16 rights = seeded(60), plain_key = seeded(61);
    SynthNcaSpec sp;
    sp.program_id = kApp;
    sp.has_rights = true;
    sp.rights_id = rights;
    sp.titlekey = plain_key;
    sp.sections = {SynthSection{1, 3, mprs_test::synth_exefs(kBaseBuild)}};
    const mprs_test::SynthNcaBuild b = mprs_test::synth_nca_build(sp);
    const std::string keys = mprs_test::write_synth_keys_file(root + "/tk");
    const KeySet ks = load_keys(keys);
    const std::string rhex = hex_encode(rights.data(), 16);

    write_file(root + "/tk/t.nsp", mprs_test::synth_nsp({{"p.nca", b.bytes},
                                                         {rhex + ".tik", mprs_test::synth_ticket(rights, mprs_test::synth_encrypt_titlekey(11, plain_key), false)}}));
    auto c = open_container(root + "/tk/t.nsp");
    CHECK_EQ(c->tickets.common.count(rhex), size_t(1));
    auto n = Nca::open(c->ncas[0].data, ks, c->tickets);
    CHECK(n->has_rights_id());
    const StoragePtr sec = n->open_section(0);
    CHECK(read_all(*sec, 0, static_cast<size_t>(sec->size())) == b.plain[0]);

    // No ticket: title.keys is the fallback (the key as stored there is the encrypted one).
    const std::string tk = mprs_test::write_synth_keys_file_with(root + "/tk2", mprs_test::SynthKeys::standard(), "title.keys",
                                                                 {{rights, mprs_test::synth_encrypt_titlekey(11, plain_key)}});
    KeySet ks2 = load_keys(keys);
    ks2.load_file(tk);
    write_file(root + "/tk/n.nsp", mprs_test::synth_nsp({{"p.nca", b.bytes}}));
    auto c2 = open_container(root + "/tk/n.nsp");
    auto n2 = Nca::open(c2->ncas[0].data, ks2, c2->tickets);
    const StoragePtr sec2 = n2->open_section(0);
    CHECK(read_all(*sec2, 0, static_cast<size_t>(sec2->size())) == b.plain[0]);
    auto n3 = Nca::open(c2->ncas[0].data, ks, c2->tickets);
    CHECK(has(error_of([&] { n3->open_section(0); }), "title key"));

    // A wrong ticket key decrypts to garbage, not to the section.
    write_file(root + "/tk/w.nsp", mprs_test::synth_nsp({{"p.nca", b.bytes},
                                                         {rhex + ".tik", mprs_test::synth_ticket(rights, plain_key, false)}}));
    auto c3 = open_container(root + "/tk/w.nsp");
    auto n4 = Nca::open(c3->ncas[0].data, ks, c3->tickets);
    const StoragePtr sec4 = n4->open_section(0);
    CHECK(read_all(*sec4, 0, 64) != std::vector<uint8_t>(b.plain[0].begin(), b.plain[0].begin() + 64));
}

void test_cnmt() {
    const std::vector<uint8_t> c = cnmt_bytes(kApp + 0x800, 0x30000, 0x81, kApp, {{cid(1), 1}, {cid(2), 3}});
    const ContentMeta m = parse_cnmt(c);
    CHECK_EQ(m.title_id, kApp + 0x800);
    CHECK_EQ(m.version, 0x30000u);
    CHECK_EQ(m.type, uint8_t(MetaType::Patch));
    CHECK_EQ(m.application_id, kApp);
    CHECK_EQ(m.contents.size(), size_t(2));
    if (m.contents.size() == 2) {
        CHECK_EQ(m.contents[0].id, cid(1));
        CHECK_EQ(m.contents[0].type, uint8_t(1));
        CHECK_EQ(m.contents[1].type, uint8_t(3));
    }
    CHECK(!error_of([] { parse_cnmt(std::vector<uint8_t>(0x10)); }).empty());
}

void test_select(const Game& g) {
    const KeySet ks = load_keys(g.keys);
    auto x = open_container(g.xci);
    auto n = open_container(g.nsp);
    DumpSet s = select_titles({x.get(), n.get()}, ks, kTitleId);
    CHECK(s.base != nullptr);
    CHECK(s.have_version);
    CHECK_EQ(s.base_version, 0u);
    CHECK(s.patch != nullptr);
    CHECK_EQ(s.patch_version, 0x20000u);            // the NSP's update is newer than the XCI's
    CHECK_EQ(s.other_patches.size(), size_t(1));
    CHECK_EQ(s.aoc.size(), size_t(2));              // the DLC in both inputs counts once
    CHECK(s.input_used.size() == 2 && s.input_used[0] && s.input_used[1]);
    if (s.aoc.size() == 2) {
        CHECK_EQ(s.aoc[0].title_id, kApp + 0x1001);
        CHECK_EQ(s.aoc[1].title_id, kApp + 0x1002);
    }
    if (s.patch) {
        auto ex = s.patch->open_pfs0(0);
        CHECK_EQ(nso_read_info(*ex->open_file(*ex->find("main"))).build_id(), std::string(kNewBuild));
        // the patched section: what the builder laid out, read through IndirectStorage + AES-CTR-EX over the base
        const int rs = s.patch->romfs_section();
        CHECK_EQ(rs, 1);
        CHECK(has(error_of([&] { s.patch->open_section(rs); }), "update"));
        const StoragePtr sec = s.patch->open_section(rs, s.base.get());
        CHECK_EQ(sec->size(), u64(g.new_section.size()));
        CHECK(read_all(*sec, 0, static_cast<size_t>(sec->size())) == g.new_section);
        // reads that start and end inside entries
        std::vector<uint8_t> part = read_all(*sec, 0x4013, 0x1F7);
        CHECK(std::equal(part.begin(), part.end(), g.new_section.begin() + 0x4013));
        CHECK(s.patch->verify_section(rs, nullptr, nullptr, s.base.get()) > 0);
        auto rom = Romfs::open(s.patch->open_romfs_image(rs, s.base.get()));
        CHECK_EQ(rom->files().size(), kNewFiles.size());
        for (const RomfsFile& f : rom->files())
            for (const auto& [p, bytes] : kNewFiles)
                if (p == f.path) CHECK(read_all(*rom->open_file(f), 0, static_cast<size_t>(f.size)) == bytes);
    }
    // inputs in the other order pick the same
    DumpSet s2 = select_titles({n.get(), x.get()}, ks, kTitleId);
    CHECK_EQ(s2.patch_version, 0x20000u);
    // the update alone is not enough (a profile without a builds list keeps its old message)
    CHECK(has(error_of([&] { select_titles({n.get()}, ks, kTitleId); }), "updates are not supported"));
}

void test_storage_direct() {
    // IndirectStorage over two memory storages, no encryption.
    std::vector<uint8_t> base = pattern(0x100, 3), patch = pattern(0x40, 200);
    std::vector<IndirectEntry> e = {{0, 0, 0}, {0x30, 0x10, 1}, {0x50, 0x50, 0}};
    StoragePtr s = make_indirect_storage(e, 0x100, make_memory_storage(base), make_memory_storage(patch));
    std::vector<uint8_t> want(base);
    std::memcpy(want.data() + 0x30, patch.data() + 0x10, 0x20);
    CHECK(read_all(*s, 0, 0x100) == want);
    CHECK(read_all(*s, 0x2F, 3) == std::vector<uint8_t>(want.begin() + 0x2F, want.begin() + 0x32));
    CHECK(!error_of([&] { read_all(*s, 0xF0, 0x20); }).empty());

    // A damaged table: the entry count in the header does not match.
    std::vector<uint8_t> t(0x8000, 0);
    put32(t.data() + 4, 1);
    put64(t.data() + 8, 0x100);
    put32(t.data() + 0x4000 + 4, 2);
    put64(t.data() + 0x4000 + 8, 0x100);
    put64(t.data() + 0x4010 + 0x14, 0x80);
    u64 end = 0;
    CHECK(parse_indirect_table(t, 2, end).size() == 2);
    CHECK_EQ(end, u64(0x100));
    CHECK(has(error_of([&] { parse_indirect_table(t, 3, end); }), "damaged"));
    CHECK(has(error_of([&] { parse_indirect_table(t, 2046u * 818u + 1u, end); }), "does not read"));   // needs a second index level
    put32(t.data() + 4, 2);   // set count does not match the entries
    CHECK(has(error_of([&] { parse_indirect_table(t, 2, end); }), "damaged"));
}

struct NullReporter : Reporter {};

void test_pipeline(const Game& g) {
    const std::string out = g.dir + "/out";
    SetupOptions o;
    o.input_paths = {g.nsp, g.xci};   // any order
    o.key_files = {g.keys};
    o.out_dir = out;
    o.expected_title_id = kTitleId;
    o.expected_build_id = kNewBuild;
    o.skip_textures = true;
    o.allow_no_shaders = true;
    o.verify_input = true;

    const InputSummary in = inspect_input(o);
    CHECK(in.supported);
    if (!in.supported) std::printf("  inspect: %s\n", in.problem.c_str());
    CHECK_EQ(in.dlc_count, 2u);
    CHECK(in.notes.empty());   // the DLC in both inputs is no "unused input"
    CHECK_EQ(in.update, std::string("v131072"));   // no control NCA: the CNMT version number
    CHECK_EQ(in.build_id, std::string(kNewBuild));

    NullReporter r;
    std::string err = error_of([&] { run_setup(o, r); });
    CHECK(err.empty());
    if (!err.empty()) std::printf("  run_setup: %s\n", err.c_str());
    log_close();
    auto ex = Pfs::open(make_memory_storage(mprs_test::synth_exefs(kNewBuild)));
    for (const PfsEntry& e : ex->entries())
        CHECK(read_file(out + "/exefs/" + e.name) == read_all(*ex->open_file(e), 0, static_cast<size_t>(e.size)));
    for (const auto& [p, bytes] : kNewFiles) CHECK(read_file(out + "/romfs/" + p) == bytes);
    for (const auto& [p, bytes] : kDlcA) CHECK(read_file(out + "/aoc/" + tid_hex(kApp + 0x1001) + "/" + p) == bytes);
    for (const auto& [p, bytes] : kDlcB) CHECK(read_file(out + "/aoc/" + tid_hex(kApp + 0x1002) + "/" + p) == bytes);
    const std::vector<uint8_t> st = read_file(out + "/setup_state.tsv");
    const std::string sts(st.begin(), st.end());
    CHECK(has(sts, ("meta\taoc\t" + tid_hex(kApp + 0x1001) + ":0," + tid_hex(kApp + 0x1002) + ":0").c_str()));
    CHECK(has(sts, ("file\taoc/" + tid_hex(kApp + 0x1001) + "/aoc/item.bin").c_str()));
    CHECK(has(sts, "meta\tupdate\tv131072"));
    CHECK(!has(sts, "meta\trebuild"));

    // a second run only checks
    err = error_of([&] { run_setup(o, r); });
    CHECK(err.empty());
    log_close();

    // version messages
    SetupOptions base_only = o;
    base_only.input_paths = {g.base_only};
    base_only.out_dir = g.dir + "/out2";
    InputSummary b = inspect_input(base_only);
    CHECK(!b.supported);
    CHECK(has(b.problem, "This is not the v1.0.0 version") && has(b.problem, kNewBuild));   // the old message (no builds list)
    SetupOptions want_base = o;
    want_base.expected_build_id = kBaseBuild;
    want_base.out_dir = g.dir + "/out3";
    b = inspect_input(want_base);
    CHECK(!b.supported);
    CHECK(has(b.problem, "leave the update out"));
    // a base-only dump still works when the base is the supported build
    want_base.input_paths = {g.base_only};
    b = inspect_input(want_base);
    CHECK(b.supported);
    CHECK(b.update.empty());
    // a short build id is padded with zeros
    SetupOptions shortid = o;
    shortid.expected_build_id = std::string(kNewBuild).substr(0, 32);
    shortid.out_dir = g.dir + "/out4";
    CHECK(inspect_input(shortid).supported);

    // A profile with a builds list: the list decides, messages name what was found and what is supported.
    static const GameBuild only_new[] = {{kNewBuild, "2.0.0"}, {nullptr, nullptr}};
    static const GameBuild both[] = {{kBaseBuild, "1.0.0"}, {kNewBuild, "2.0.0"}, {nullptr, nullptr}};
    SetupOptions listed = o;
    listed.expected_build_id = SetupOptions().expected_build_id;   // the default, so the list is used
    listed.builds = only_new;
    listed.out_dir = g.dir + "/out5";
    b = inspect_input(listed);
    CHECK(b.supported);
    CHECK_EQ(b.version, std::string("2.0.0"));
    listed.input_paths = {g.base_only};
    b = inspect_input(listed);
    CHECK(!b.supported);
    CHECK(has(b.problem, "not supported: found the base game") && has(b.problem, "Supported: v2.0.0"));
    listed.builds = both;
    b = inspect_input(listed);
    CHECK(b.supported);
    CHECK_EQ(b.version, std::string("1.0.0"));

    // Rebuild into the same folder: base only, then base + update again. No stale files, no stale meta.
    SetupOptions down = o;
    down.input_paths = {g.base_only};
    down.expected_build_id = kBaseBuild;
    err = error_of([&] { run_setup(down, r); });
    log_close();
    CHECK(err.empty());
    if (!err.empty()) std::printf("  rebuild to base: %s\n", err.c_str());
    CHECK(!fs::exists(out + "/romfs/data/c.bin"));
    CHECK(!fs::exists(out + "/aoc"));
    for (const auto& [p, bytes] : kBaseFiles) CHECK(read_file(out + "/romfs/" + p) == bytes);
    auto bex = Pfs::open(make_memory_storage(mprs_test::synth_exefs(kBaseBuild)));
    CHECK(read_file(out + "/exefs/main") == read_all(*bex->open_file(*bex->find("main")), 0, static_cast<size_t>(bex->find("main")->size)));
    {
        const std::vector<uint8_t> st2 = read_file(out + "/setup_state.tsv");
        const std::string t2(st2.begin(), st2.end());
        CHECK(!has(t2, "meta\tupdate"));
        CHECK(!has(t2, "meta\taoc"));
        CHECK(!has(t2, "meta\trebuild"));
        CHECK(!has(t2, "data/c.bin"));
    }
    err = error_of([&] { run_setup(o, r); });
    log_close();
    CHECK(err.empty());
    for (const auto& [p, bytes] : kNewFiles) CHECK(read_file(out + "/romfs/" + p) == bytes);
    for (const auto& [p, bytes] : kDlcB) CHECK(read_file(out + "/aoc/" + tid_hex(kApp + 0x1002) + "/" + p) == bytes);
}

void test_notes_and_keys(const Game& g) {
    // An input for another game is named in the notes; a DLC whose title key is missing is a keys problem.
    const u64 other = 0x0100000000AAA000ull;
    write_file(g.dir + "/other_dlc.nsp",
               mprs_test::synth_nsp({{cid(20) + ".nca", dlc_nca(other + 0x1001, kDlcB)},
                                     {cid(21) + ".cnmt.nca", meta_nca(other + 0x1001, cnmt_bytes(other + 0x1001, 0, 0x82, other, {{cid(20), 2}}))}}));
    SetupOptions o;
    o.input_paths = {g.base_only, g.dir + "/other_dlc.nsp"};
    o.key_files = {g.keys};
    o.out_dir = g.dir + "/out_notes";
    o.expected_title_id = kTitleId;
    o.expected_build_id = kBaseBuild;
    o.skip_textures = true;
    o.allow_no_shaders = true;
    InputSummary in = inspect_input(o);
    CHECK(in.supported);
    CHECK_EQ(in.notes.size(), size_t(1));
    if (!in.notes.empty()) CHECK(has(in.notes[0], "other_dlc.nsp"));

    SynthNcaSpec sp;
    sp.content_type = 5;
    sp.program_id = kApp + 0x1003;
    sp.has_rights = true;
    sp.rights_id = seeded(70);
    sp.titlekey = seeded(71);
    sp.sections = {SynthSection{0, 3, mprs_test::synth_romfs(kDlcB)}};
    write_file(g.dir + "/locked_dlc.nsp",
               mprs_test::synth_nsp({{cid(22) + ".nca", mprs_test::synth_nca(sp)},
                                     {cid(23) + ".cnmt.nca", meta_nca(kApp + 0x1003, cnmt_bytes(kApp + 0x1003, 0, 0x82, kApp, {{cid(22), 2}}))}}));
    o.input_paths = {g.base_only, g.dir + "/locked_dlc.nsp"};
    NullReporter r;
    Fail kind = Fail::Generic;
    std::string msg;
    try {
        run_setup(o, r);
    } catch (const Error& e) {
        kind = e.kind;
        msg = e.what();
    }
    log_close();
    CHECK(kind == Fail::Keys);
    CHECK(has(msg, "DLC") && has(msg, "title key"));
}

void test_damaged_patch(const std::string& root) {
    (void)root;
    const Game g = make_game("damaged", true);
    SetupOptions o;
    o.input_paths = {g.xci, g.nsp};
    o.key_files = {g.keys};
    o.out_dir = g.dir + "/out";
    o.expected_title_id = kTitleId;
    o.expected_build_id = kNewBuild;
    o.skip_textures = true;
    o.allow_no_shaders = true;
    NullReporter r;
    const std::string err = error_of([&] { run_setup(o, r); });
    log_close();
    CHECK(has(err, "damaged"));
}

}  // namespace

int main() {
    const std::string root = tmp_dir("all");
    test_ticket_titlekey(root);
    test_cnmt();
    test_storage_direct();
    const Game g = make_game("game");
    test_select(g);
    test_pipeline(g);
    test_damaged_patch(root);
    test_notes_and_keys(g);
    return TEST_SUMMARY("test_update");
}
