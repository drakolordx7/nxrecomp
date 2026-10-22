// SPDX-License-Identifier: GPL-3.0-or-later
// Builders for synthetic Switch dumps (NCA, ticket, ExeFS, NSP, XCI) with a fake key set, used by test_nca and test_container.
// They write the formats the way src/nca.cpp, src/container.cpp and src/keys.cpp read them, and they carry their own AES-128,
// AES-XTS (Nintendo) and AES-CTR, so they do not depend on the project's crypto (synth*.cpp is linked into every test exe,
// including the ones that do not link mprs_crypto).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "synth.h"

namespace mprs_test {

using Bytes16 = std::array<uint8_t, 16>;

// The fake key set. standard() returns fixed pseudo-random values; the derived keys (header key, key area key, titlekek) are
// computed from these with the usual formulas (see synth_nca.cpp).
struct SynthKeys {
    Bytes16 master_key_00{}, master_key_0a{};
    Bytes16 aes_kek_generation_source{}, aes_key_generation_source{};
    Bytes16 titlekek_source{}, key_area_key_application_source{}, header_kek_source{};
    std::array<uint8_t, 32> header_key_source{};
    static SynthKeys standard();
};

// Writes <dir>\prod.keys with exactly the eight entries above (the standard keys) and returns its path.
std::string write_synth_keys_file(const std::string& dir);
// Same with the given keys, a file name, and extra "rights id -> encrypted title key" lines (title.keys style).
std::string write_synth_keys_file_with(const std::string& dir, const SynthKeys& keys, const std::string& file_name,
                                       const std::vector<std::pair<Bytes16, Bytes16>>& extra_title_keys = {});

// Encrypts a plain title key the way a ticket stores it (with the titlekek of the given NCA key generation, 1 or 11).
Bytes16 synth_encrypt_titlekey(uint8_t key_generation, const Bytes16& plain_titlekey);

// AES-128 single block encryption with the project-independent implementation (used by the tests to check it against FIPS-197).
void synth_aes128_encrypt_block(const uint8_t key[16], uint8_t block[16]);

struct SynthSection {
    uint8_t fs_type = 1;         // 0 RomFS (IVFC, hash_type 3), 1 PFS0 (hierarchical SHA-256, hash_type 2)
    uint8_t enc_type = 3;        // 1 none, 3 AES-CTR
    std::vector<uint8_t> data;   // the PFS0 image (fs 1) or the RomFS image (fs 0)
};

// Damage applied to the plain section before it is encrypted (MasterHash changes the master hash in the section header).
struct SynthCorruption {
    enum class Where { HashLayer, DataFirstByte, DataLastByte, MasterHash };
    int section = 0;
    Where where = Where::HashLayer;
};

struct SynthNcaSpec {
    uint8_t content_type = 0;            // 0 Program, 1 Meta, 2 Control, 3 Manual, 4 Data, 5 PublicData
    uint64_t program_id = 0x010012101468C000ull;
    uint32_t content_index = 0;
    uint8_t key_generation = 11;         // NCA value: 1 = master key 00, 11 = master key 0a (the only ones the keys file has)
    uint8_t key_area_kind = 0;           // 0 application (the only kind the builder derives)
    bool has_rights = false;
    Bytes16 rights_id{};                 // used when has_rights
    Bytes16 titlekey{};                  // plain title key; the ticket stores it encrypted (synth_ticket) when has_rights
    std::vector<SynthSection> sections;  // up to 4, placed in order from 0xC00
    std::vector<SynthCorruption> corrupt;
    bool truncate = false;
    size_t truncate_to = 0;              // used when truncate
    char magic_digit = '3';              // '3' NCA3, '2' NCA2 (per-section header encryption), '0' NCA0 (rejected)
};

// The NCA bytes plus the plain (decrypted, post-corruption) section bytes, index = section index (empty for absent sections).
struct SynthNcaBuild {
    std::vector<uint8_t> bytes;
    std::vector<std::vector<uint8_t>> plain;
};
SynthNcaBuild synth_nca_build(const SynthNcaSpec& spec);
std::vector<uint8_t> synth_nca(const SynthNcaSpec& spec);

// A 0x2C0-byte RSA-2048-SHA256 ticket (signature type 0x10004). The title key block holds titlekey_encrypted (16 bytes first);
// personalized sets the title key type to 1. The rights id is at ticket offset 0x2A0, the switchbrew position.
std::vector<uint8_t> synth_ticket(const Bytes16& rights_id, const Bytes16& titlekey_encrypted, bool personalized);

// An ExeFS PFS0: main, rtld, sdk, subsdk0 (NSO0 files of 0x200 bytes, module id = build id, compressed sizes 0x20) and main.npdm.
// build_id_40_hex is 40 hex digits (20 bytes).
std::vector<uint8_t> synth_exefs(const std::string& build_id_40_hex);

// A Meta NCA (content type 1) whose section 0 is a PFS0 with "Application_<titleid>.cnmt" (u64 title id, u32 version, u8 type 0x80).
std::vector<uint8_t> synth_cnmt_nca(uint64_t title_id, uint32_t version);

// NSP = PFS0 of the given .nca / .tik entries.
std::vector<uint8_t> synth_nsp(const SynthFiles& entries);
// XCI: 0x200-byte header ("HEAD" at 0x100, root HFS0 offset 0x200 at 0x138) and a root HFS0 holding "update" (empty) and, when
// include_secure, "secure" (an HFS0 of the given entries).
std::vector<uint8_t> synth_xci(const SynthFiles& secure_entries, bool include_secure = true);

struct SynthDump {
    std::string path;                    // the .nsp or .xci file
    std::string keys_file_path;          // prod.keys with the standard synthetic keys
    std::string build_id;                // expected main NSO build id, 40 upper-case hex digits
    uint64_t program_id = 0;
    SynthFiles romfs_files;              // expected RomFS contents (paths with '/')
};
// kind "nsp" or "xci". Writes the dump and the keys file into dir (created if missing; "" = a folder under the temp directory).
// Contents: the program NCA (section 0 = ExeFS, section 1 = RomFS) and the meta NCA (version 0). use_ticket gives the program NCA a
// rights id and puts its ticket in the dump; otherwise the program NCA uses the key area.
SynthDump make_synth_dump(const char* kind, uint64_t program_id, const std::string& build_id_40_hex, const SynthFiles& romfs_files,
                          bool use_ticket, const std::string& dir = "");

// The rights id and plain title key make_synth_dump uses for its ticket.
Bytes16 synth_dump_rights_id();
Bytes16 synth_dump_titlekey();

}  // namespace mprs_test
