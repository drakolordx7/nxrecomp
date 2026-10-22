// SPDX-License-Identifier: GPL-3.0-or-later
// NCA (Nintendo Content Archive) reader: header decryption, key selection, section decryption, hash-tree verification.
// Format knowledge: https://switchbrew.org/wiki/NCA and hactool (ISC, SciresM) / LibHac (MIT) read as documentation; written fresh.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "keys.h"
#include "pfs.h"
#include "romfs.h"

namespace mprs {

enum class NcaContentType : u8 { Program = 0, Meta = 1, Control = 2, Manual = 3, Data = 4, PublicData = 5 };

// Title keys found in tickets inside the container (rights id as 32 lower-case hex digits -> the title key as stored in the
// ticket, still encrypted with the titlekek). Personalized tickets (RSA-wrapped to one console) cannot be used.
struct TicketKeys {
    std::map<std::string, Key128> common;
    std::vector<std::string> personalized;
};

struct NcaSection {
    int index = 0;
    u64 offset = 0, size = 0;          // inside the NCA file
    u8 fs_type = 0;                    // 0 RomFS, 1 PFS0
    u8 hash_type = 0;                  // 2 hierarchical SHA-256 (PFS0), 3 hierarchical integrity (IVFC, RomFS)
    u8 enc_type = 0;                   // 1 none, 2 AES-XTS, 3 AES-CTR, 4 AES-CTR-EX (patch)
    std::array<u8, 8> ctr_hi{};        // upper half of the CTR counter (section header +0x140 reversed)
    std::array<u8, 0x200> header{};    // the decrypted section header
};

using ProgressFn = std::function<void(u64 done, u64 total)>;   // bytes
using CancelFn = std::function<bool()>;

class Nca {
public:
    // Decrypts the header with the header key. Throws Error(Keys) if the result is not an NCA ("these keys do not belong to this
    // dump or are too old"); Error(Input) for a file that is too small or has a malformed header. Section keys are resolved lazily
    // (open_* functions), so listing NCAs of titles whose keys you lack still works.
    static std::unique_ptr<Nca> open(StoragePtr file, const KeySet& keys, const TicketKeys& tickets);

    NcaContentType content_type() const { return type_; }
    u64 program_id() const { return program_id_; }
    u64 content_size() const { return content_size_; }
    u32 content_index() const { return content_index_; }
    int key_generation() const { return key_gen_; }              // master key index (0-based)
    bool has_rights_id() const { return has_rights_; }
    const std::array<u8, 16>& rights_id() const { return rights_id_; }
    const NcaSection* section(int i) const { return (i >= 0 && i < 4 && sec_[i].size) ? &sec_[i] : nullptr; }
    const StoragePtr& file() const { return file_; }

    // The decrypted bytes of a whole section (what the hash layers describe). Throws Error(Keys) when the key is unavailable
    // (no ticket / title key), Error(Input) for unsupported encryption (patch NCAs).
    StoragePtr open_section(int i) const;
    // PFS0 file system of a section (hash_type 2). The returned Pfs's entry offsets are relative to the PFS0 start.
    std::unique_ptr<Pfs> open_pfs0(int i) const;
    // RomFS image of a section (hash_type 3): the data level of the integrity tree.
    StoragePtr open_romfs_image(int i) const;
    // Verifies the whole hash tree of a section against the stored hashes (reads and hashes the entire section data).
    // Throws Error(Input) naming the damaged area on mismatch. Returns the number of bytes hashed.
    u64 verify_section(int i, const ProgressFn& progress, const CancelFn& cancelled) const;

private:
    Key128 section_key(const NcaSection& s) const;
    StoragePtr file_;
    const KeySet* keys_ = nullptr;
    TicketKeys tickets_;
    NcaContentType type_ = NcaContentType::Program;
    u64 program_id_ = 0, content_size_ = 0;
    u32 content_index_ = 0;
    int key_gen_ = 0;
    int key_area_kind_ = 0;
    bool has_rights_ = false;
    std::array<u8, 16> rights_id_{};
    std::array<u8, 0x40> key_area_{};     // encrypted
    NcaSection sec_[4];
};

}  // namespace mprs
