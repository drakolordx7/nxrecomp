// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the container layer (src/container.cpp: NSP / XCI detection, tickets, find_program) on synthetic dumps from
// synth_nca.cpp.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "common.h"
#include "container.h"
#include "keys.h"
#include "nca.h"
#include "nso.h"
#include "romfs.h"
#include "synth.h"
#include "synth_nca.h"

using namespace mprs;
using mprs_test::SynthFiles;
using mprs_test::SynthKeys;

namespace {

constexpr uint64_t kProgramId = 0x010012101468C000ull;
const char* const kTitleId = "010012101468C000";
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
    const auto p = std::filesystem::temp_directory_path() / (std::string("mprs_test_container_") + name);
    std::filesystem::create_directories(p);
    return p.string();
}

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 5 + (i >> 9));
    return v;
}

void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::string hex16(const std::array<uint8_t, 16>& b) { return hex_encode(b.data(), 16); }

SynthFiles romfs_files() {
    return {{"Worlds/MP1/Test.pak", pattern(3000, 9)}, {"readme.txt", std::vector<uint8_t>{'o', 'k'}}};
}

// Returns true when the container's main program is found and its main NSO has the expected build id.
bool program_ok(const Container& c, const KeySet& keys, uint64_t title_id_value) {
    (void)title_id_value;
    const DumpTitle t = find_program(c, keys, kTitleId);
    if (!t.program) return false;
    auto exefs = t.program->open_pfs0(0);
    const PfsEntry* main = exefs->find("main");
    if (!main) return false;
    return nso_read_info(*exefs->open_file(*main)).build_id() == kBuildId;
}

}  // namespace

int main() {
    const std::string root = tmp_dir("all");
    const SynthFiles files = romfs_files();
    const mprs_test::SynthDump nsp = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, root + "/nsp");
    const mprs_test::SynthDump xci = mprs_test::make_synth_dump("xci", kProgramId, kBuildId, files, false, root + "/xci");
    const mprs_test::SynthDump nsp_tik = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, true, root + "/nsp_tik");
    const mprs_test::SynthDump xci_tik = mprs_test::make_synth_dump("xci", kProgramId, kBuildId, files, true, root + "/xci_tik");
    const mprs_test::SynthDump other =
        mprs_test::make_synth_dump("nsp", 0x0100000000001000ull, kBuildId, files, false, root + "/other");
    const mprs_test::SynthDump update =
        mprs_test::make_synth_dump("nsp", kProgramId + 0x800, kBuildId, files, false, root + "/update");
    const KeySet keys = [&] {
        KeySet k;
        k.load_file(nsp.keys_file_path);
        return k;
    }();

    // ---- NSP and XCI detection ----
    {
        auto c = open_container(nsp.path);
        CHECK_EQ(c->kind, std::string("NSP"));
        CHECK_EQ(c->ncas.size(), size_t(2));                 // program + meta
        CHECK(c->tickets.common.empty());
        CHECK(c->tickets.personalized.empty());
    }
    {
        auto c = open_container(xci.path);
        CHECK_EQ(c->kind, std::string("XCI"));
        CHECK_EQ(c->ncas.size(), size_t(2));
        CHECK(c->tickets.common.empty());
    }

    // ---- tickets inside the dump ----
    // The dump's ticket stores the title key under its rights id. The reader must index it under the same rights id the NCA
    // carries (ticket offset 0x2A0, body 0x160). See the test report: src/container.cpp reads body 0x15C instead.
    {
        const std::string want = hex16(mprs_test::synth_dump_rights_id());
        auto c = open_container(nsp_tik.path);
        CHECK_EQ(c->kind, std::string("NSP"));
        CHECK_EQ(c->tickets.common.size(), size_t(1));
        CHECK(c->tickets.common.count(want) == 1);
        if (c->tickets.common.count(want) != 1)
            for (const auto& kv : c->tickets.common)
                std::printf("  reader indexed the ticket title key under %s; the NCA's rights id is %s\n", kv.first.c_str(), want.c_str());
        auto cx = open_container(xci_tik.path);
        CHECK_EQ(cx->tickets.common.size(), size_t(1));
        CHECK(cx->tickets.common.count(want) == 1);
    }

    // ---- find_program: base game, NSP and XCI ----
    for (const mprs_test::SynthDump* d : {&nsp, &xci}) {
        auto c = open_container(d->path);
        const DumpTitle t = find_program(*c, keys, kTitleId);
        CHECK(t.program != nullptr);
        CHECK_EQ(t.title_id, kProgramId);
        CHECK_EQ(t.title_version, 0u);
        CHECK(t.have_version);
        CHECK(!t.is_update);
        CHECK(program_ok(*c, keys, kProgramId));
        if (t.program) {
            auto rf = Romfs::open(t.program->open_romfs_image(1));
            CHECK_EQ(rf->files().size(), files.size());
            CHECK(t.program->verify_section(0, nullptr, nullptr) > 0);
            CHECK(t.program->verify_section(1, nullptr, nullptr) > 0);
        }
    }

    // ---- find_program: program with a ticket (rights id path) ----
    // Opening the dump's program NCA through the ticket must work. Expected to fail until container.cpp reads the rights id at
    // body offset 0x160 (see the report).
    {
        auto c = open_container(nsp_tik.path);
        const DumpTitle t = find_program(*c, keys, kTitleId);
        CHECK(t.program != nullptr);
        if (t.program) {
            const Outcome o = run([&] { (void)t.program->open_pfs0(0); });
            CHECK(!o.threw);
            if (o.threw) std::printf("  ticket path: %s\n", o.msg.c_str());
        }
    }

    // ---- find_program: messages for the wrong game and for an update ----
    {
        auto c = open_container(other.path);
        const Outcome o = run([&] { find_program(*c, keys, kTitleId); });
        CHECK(o.threw && o.kind == Fail::Input);
        CHECK(o.msg.find("different game") != std::string::npos);
    }
    {
        auto c = open_container(update.path);
        const Outcome o = run([&] { find_program(*c, keys, kTitleId); });
        CHECK(o.threw && o.kind == Fail::Input);
        CHECK(o.msg.find("update") != std::string::npos);
    }

    // ---- wrong keys ----
    {
        SynthKeys bad = SynthKeys::standard();
        bad.header_kek_source[0] ^= 0xFF;
        KeySet bad_keys;
        bad_keys.load_file(mprs_test::write_synth_keys_file_with(root + "/bad", bad, "prod.keys"));
        auto c = open_container(nsp.path);
        const Outcome o = run([&] { find_program(*c, bad_keys, kTitleId); });
        CHECK(o.threw && o.kind == Fail::Keys);
        if (!(o.threw && o.kind == Fail::Keys)) std::printf("  got: %s\n", o.msg.c_str());
    }

    // ---- garbage, empty, and XCI without a secure partition ----
    {
        const std::string garbage = root + "/garbage.bin";
        std::vector<uint8_t> junk(0x1000);
        for (size_t i = 0; i < junk.size(); ++i) junk[i] = uint8_t(i * 37 + 11);
        write_file(garbage, junk);
        const Outcome o = run([&] { open_container(garbage); });
        CHECK(o.threw && o.kind == Fail::Input);
    }
    {
        const std::string empty = root + "/empty.nsp";
        write_file(empty, {});
        const Outcome o = run([&] { open_container(empty); });
        CHECK(o.threw && o.kind == Fail::Input);
    }
    {
        const std::string empty_nsp = root + "/empty_pfs.nsp";
        write_file(empty_nsp, mprs_test::synth_nsp({}));
        const Outcome o = run([&] { open_container(empty_nsp); });
        CHECK(o.threw && o.kind == Fail::Input);
    }
    {
        const std::string no_secure = root + "/no_secure.xci";
        write_file(no_secure, mprs_test::synth_xci({}, false));
        const Outcome o = run([&] { open_container(no_secure); });
        CHECK(o.threw && o.kind == Fail::Input);
    }

    std::printf("(dumps in: %s)\n", root.c_str());
    return TEST_SUMMARY("test_container");
}
