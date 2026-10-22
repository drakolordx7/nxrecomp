// SPDX-License-Identifier: GPL-3.0-or-later
// Reads the real dump (skipped when absent): container, program NCA, title id, build id, hash trees, RomFS listing.
// Env: MPRS_DUMP (nsp path), MPRS_KEYS (prod.keys), MPRS_TITLEKEYS optional, MPRS_VERIFY=1 for the full hash-tree pass.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "nso.h"

#include "check.h"
#include "container.h"
#include "fileio.h"
#include "log.h"

using namespace mprs;

static int run();
int main() {
    try { return run(); } catch (const std::exception& e) { std::printf("EXCEPTION: %s\n", e.what()); return 3; }
}
static int run() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* dump_env = std::getenv("MPRS_DUMP");
    const char* keys_env = std::getenv("MPRS_KEYS");
    std::string dump = dump_env ? dump_env
                                : R"(C:\Users\drakolord\Downloads\Metroid Prime Remastered (NSP)(eShop).part1\Metroid Prime Remastered [010012101468C000].nsp)";
    std::string keys_path = keys_env ? keys_env : R"(D:\AI\Active\Metroid-Prime\game\keys\prod.keys)";
    if (!path_exists(dump) || !path_exists(keys_path)) {
        std::printf("test_realdump: skipped (dump or keys not found)\n");
        return 0;
    }
    KeySet keys;
    keys.load_file(keys_path);
    if (std::getenv("MPRS_TITLEKEYS")) keys.load_file(std::getenv("MPRS_TITLEKEYS"));
    std::printf("keys: %s\n", keys.describe().c_str());

    auto c = open_container(dump);
    std::printf("container %s, %zu NCAs, %zu common tickets\n", c->kind.c_str(), c->ncas.size(), c->tickets.common.size());
    CHECK_EQ(c->kind, std::string("NSP"));
    DumpTitle t = find_program(*c, keys, "010012101468C000");
    CHECK_EQ(t.title_id, 0x010012101468C000ull);
    CHECK(t.have_version);
    CHECK_EQ(t.title_version, 0u);
    std::printf("program NCA: key generation %d, rights id %s\n", t.program->key_generation(), t.program->has_rights_id() ? "yes" : "no");

    for (int i = 0; i < 4; ++i) {
        const NcaSection* s = t.program->section(i);
        if (s) std::printf("section %d: offset %llu size %llu fs %u hash %u enc %u\n", i, (unsigned long long)s->offset,
                           (unsigned long long)s->size, s->fs_type, s->hash_type, s->enc_type);
    }
    auto exefs = t.program->open_pfs0(0);
    for (auto& e : exefs->entries()) std::printf("  exefs %s %llu\n", e.name.c_str(), (unsigned long long)e.size);
    const PfsEntry* m = exefs->find("main");
    CHECK(m != nullptr);
    if (m) {
        NsoInfo ni = nso_read_info(*exefs->open_file(*m));
        std::printf("main build id %s\n", ni.build_id().c_str());
        CHECK_EQ(ni.build_id(), std::string("67C9CA1428222752F951021464A7B3597EB98FEC"));
    }
    auto img = t.program->open_romfs_image(1);
    auto rom = Romfs::open(img);
    std::printf("romfs: %zu files, %zu dirs, %llu bytes\n", rom->files().size(), rom->dirs().size(), (unsigned long long)rom->total_file_bytes());
    CHECK(rom->files().size() > 300);

    auto t0 = std::chrono::steady_clock::now();
    t.program->verify_section(0, nullptr, nullptr);
    std::printf("exefs hash table OK\n");
    if (std::getenv("MPRS_VERIFY")) {
        u64 last = 0;
        u64 n = t.program->verify_section(1, [&](u64 d, u64 tot) {
            if (d - last > (1ull << 30)) { last = d; std::printf("  verified %.1f of %.1f GB\n", d / 1e9, tot / 1e9); }
        }, nullptr);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("romfs integrity tree OK: %.2f GB hashed in %.1f s\n", n / 1e9, s);
    }
    return TEST_SUMMARY("test_realdump");
}
