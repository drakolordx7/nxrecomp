// SPDX-License-Identifier: GPL-3.0-or-later
// The player's Switch keys (prod.keys, title.keys). Values are read into memory only: never logged, printed, copied or written.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "common.h"

namespace mprs {

class KeySet {
public:
    // Reads "name = hex" lines (blank lines and lines starting with '#' or ';' ignored; names lower-cased, spaces trimmed). Used
    // for both prod.keys and title.keys (a title key line is "<32 hex digit rights id> = <32 hex digit key>"). Throws Error(Keys)
    // if the file cannot be read; malformed lines are skipped (counted, not echoed).
    void load_file(const std::string& utf8_path);

    size_t count() const { return keys_.size(); }
    size_t title_key_count() const { return title_keys_.size(); }
    bool has(const std::string& name) const { return keys_.count(name) != 0; }

    // 32 bytes (two AES-128 keys). Uses "header_key" if present, else derives it from header_kek_source / header_key_source /
    // master_key_00 / aes_*_generation_source. Throws Error(Keys) naming the missing key *name* (never a value).
    std::array<u8, 32> header_key() const;
    // kind 0 = application, 1 = ocean, 2 = system; generation = master key index (0-based, = NCA key generation - 1, min 0).
    // Uses "key_area_key_<kind>_<gg>" if present, else derives it from the source and master_key_<gg>.
    Key128 key_area_key(int kind, int generation) const;
    // Uses "titlekek_<gg>" if present, else AES-ECB-decrypts titlekek_source with master_key_<gg>.
    Key128 titlekek(int generation) const;
    // The title key (as stored in title.keys, i.e. still encrypted with the titlekek) for a rights id, if known.
    bool title_key(const std::array<u8, 16>& rights_id, Key128& out) const;

    // What is loaded, without values: "412 keys, master keys 00..15, 19 title keys".
    std::string describe() const;

private:
    bool get(const std::string& name, u8* out, size_t n) const;
    Key128 master_key(int generation) const;
    std::map<std::string, std::vector<u8>> keys_;
    std::map<std::string, Key128> title_keys_;   // key: 32 hex digits lower case
};

struct KeyFileCandidate {
    std::string path;     // full path of a prod.keys
    std::string source;   // "yuzu", "Eden", "Ryujinx", "next to the program", ...
};
// Looks for prod.keys in the usual places (%APPDATA%\yuzu\keys, \eden\keys, \suyu\keys, \sudachi\keys, \citron\keys,
// \Ryujinx\system, %USERPROFILE%\.switch, next to the executable and in .\keys). Only checks that files exist; reads nothing.
std::vector<KeyFileCandidate> find_key_files();
// title.keys next to a prod.keys path, or "" if none.
std::string sibling_title_keys(const std::string& prod_keys_path);

}  // namespace mprs
