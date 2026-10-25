// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for keys.cpp: the prod.keys / title.keys parser, the key derivations, and the key file search.
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "check.h"
#include "project_root.h"
#include "common.h"
#include "crypto.h"
#include "keys.h"

using namespace mprs;

namespace {

const std::string kRealProdStr = mprs_test::project_path("game/keys/prod.keys");
const std::string kRealTitleStr = mprs_test::project_path("game/keys/title.keys");
const char* kRealProd = kRealProdStr.c_str();
const char* kRealTitle = kRealTitleStr.c_str();

std::string temp_path(const std::string& name) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / ("mprs_test_keys_" + name);
    return narrow(p.wstring());
}

void write_text(const std::string& utf8_path, const std::string& content) {
    std::ofstream f(widen(utf8_path), std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string read_all(const std::string& utf8_path) {
    std::ifstream f(widen(utf8_path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string lo(const Key128& k) { return hex_encode(k.data(), k.size()); }
std::string up(const Key128& k) { return hex_encode(k.data(), k.size(), true); }

// Deterministic, random-looking fixed bytes.
Key128 pattern(u8 seed) {
    Key128 k{};
    for (size_t i = 0; i < k.size(); ++i) {
        k[i] = static_cast<u8>((seed * 37u + i * 11u + i * i * 5u) ^ 0xA5u);
    }
    return k;
}

std::array<u8, 32> pattern32(u8 seed) {
    std::array<u8, 32> k{};
    const Key128 a = pattern(seed), b = pattern(static_cast<u8>(seed + 100));
    std::copy(a.begin(), a.end(), k.begin());
    std::copy(b.begin(), b.end(), k.begin() + 16);
    return k;
}

// True if f throws an mprs::Error of the given kind; msg receives the message.
template <class F>
bool throws_error(F&& f, Fail kind, std::string& msg) {
    try {
        f();
    } catch (const Error& e) {
        msg = e.what();
        return e.kind == kind;
    } catch (...) {
        return false;
    }
    return false;
}

// Synthetic key material for the derivation tests.
struct Synthetic {
    Key128 mk00 = pattern(1);
    Key128 mk0a = pattern(2);
    Key128 aes_kek_src = pattern(3);
    Key128 aes_key_src = pattern(4);
    Key128 titlekek_src = pattern(5);
    Key128 kaak_app_src = pattern(6);
    Key128 header_kek_src = pattern(7);
    std::array<u8, 32> header_src = pattern32(8);
};

struct Expected {
    Key128 kaak_app_0a{};
    Key128 titlekek_0a{};
    std::array<u8, 32> header{};
};

// The formulas, written out straight-line with Aes128Ecb, independent of keys.cpp.
Expected expected_values(const Synthetic& s) {
    Expected e{};
    // generation 0x0a
    Key128 kek_0a{};
    Aes128Ecb(s.mk0a.data()).decrypt(s.aes_kek_src.data(), kek_0a.data(), 16);
    Key128 src_kek_0a{};
    Aes128Ecb(kek_0a.data()).decrypt(s.kaak_app_src.data(), src_kek_0a.data(), 16);
    Aes128Ecb(src_kek_0a.data()).decrypt(s.aes_key_src.data(), e.kaak_app_0a.data(), 16);
    Aes128Ecb(s.mk0a.data()).decrypt(s.titlekek_src.data(), e.titlekek_0a.data(), 16);
    // header key, from master_key_00
    Key128 kek_00{};
    Aes128Ecb(s.mk00.data()).decrypt(s.aes_kek_src.data(), kek_00.data(), 16);
    Key128 header_kek{};
    Aes128Ecb(kek_00.data()).decrypt(s.header_kek_src.data(), header_kek.data(), 16);
    Key128 header_key_key{};
    Aes128Ecb(header_kek.data()).decrypt(s.aes_key_src.data(), header_key_key.data(), 16);
    Aes128Ecb(header_key_key.data()).decrypt(s.header_src.data(), e.header.data(), 32);
    return e;
}

// A prod-style file with the sources and two master keys. `omit` leaves one name out.
std::string synthetic_text(const Synthetic& s, const std::string& omit = "") {
    std::string t;
    const auto add = [&](const std::string& name, const std::string& value) {
        if (name != omit) t += name + " = " + value + "\n";
    };
    add("master_key_00", lo(s.mk00));
    add("master_key_0a", lo(s.mk0a));
    add("aes_kek_generation_source", lo(s.aes_kek_src));
    add("aes_key_generation_source", lo(s.aes_key_src));
    add("titlekek_source", lo(s.titlekek_src));
    add("key_area_key_application_source", lo(s.kaak_app_src));
    add("header_kek_source", lo(s.header_kek_src));
    add("header_key_source", hex_encode(s.header_src.data(), 32));
    return t;
}

std::vector<std::string> all_secret_hex(const Synthetic& s) {
    const Expected e = expected_values(s);
    return {lo(s.mk00),   lo(s.mk0a),          lo(s.aes_kek_src), lo(s.aes_key_src),
            lo(s.titlekek_src), lo(s.kaak_app_src), lo(s.header_kek_src),
            hex_encode(s.header_src.data(), 32), lo(e.kaak_app_0a), lo(e.titlekek_0a),
            hex_encode(e.header.data(), 32)};
}

void test_parser() {
    Key128 tk_val = pattern(12);
    Key128 titlekek00 = pattern(11);
    Key128 last = pattern(13);

    std::string text;
    text += "# a comment line\r\n";
    text += "; another comment\r\n";
    text += "\r\n";
    text += "   MASTER_KEY_00 \t=\t  " + up(pattern(1)) + "  \r\n";   // spaces around '=', upper case
    text += "aes_kek_generation_source=" + up(pattern(3)) + "\n";     // no spaces
    text += "this line is not a key\n";                               // no '=' : skipped
    text += "bad_value = 0x12\n";                                     // not hex : skipped
    text += "odd_length = 123\n";                                     // odd digit count : skipped
    text += "too_long = " + std::string(258, '0') + "\n";             // over 256 digits : skipped
    text += "max_len = " + std::string(256, 'f') + "\n";              // exactly 256 digits : kept
    text += "TITLEKEK_00 = " + up(titlekek00) + "\n";                 // name lower-cased
    text += "empty_value =\n";                                        // empty value : skipped
    text += "=novalue\n";                                             // empty name : skipped
    text += "  ; indented comment\n";
    text += "010012101468C000000000000000000B = " + up(tk_val) + "\n";   // title key line
    text += "last_one = " + up(last);                                  // no trailing newline

    const std::string path = temp_path("parser.keys");
    write_text(path, text);
    KeySet ks;
    ks.load_file(path);
    std::filesystem::remove(widen(path));

    CHECK_EQ(ks.count(), static_cast<size_t>(5));          // master_key_00, aes_kek..., max_len, titlekek_00, last_one
    CHECK_EQ(ks.title_key_count(), static_cast<size_t>(1));
    CHECK(ks.has("master_key_00"));
    CHECK(ks.has("aes_kek_generation_source"));
    CHECK(ks.has("max_len"));
    CHECK(ks.has("last_one"));
    CHECK(!ks.has("MASTER_KEY_00"));                       // names are stored lower-case
    CHECK(!ks.has("bogus_value"));
    CHECK(!ks.has("odd_length"));
    CHECK(!ks.has("too_long"));
    CHECK(!ks.has("empty_value"));

    CHECK(ks.titlekek(0) == titlekek00);                   // explicit titlekek_00, parsed from upper-case hex

    std::array<u8, 16> rid{};
    CHECK(hex_decode("010012101468c000000000000000000b", rid.data(), rid.size()));
    Key128 found{};
    CHECK(ks.title_key(rid, found));
    CHECK(found == tk_val);

    // Missing file.
    std::string msg;
    const std::string missing = temp_path("does_not_exist.keys");
    std::filesystem::remove(widen(missing));
    CHECK(throws_error([&] { KeySet k; k.load_file(missing); }, Fail::Keys, msg));
    CHECK(msg.find("Cannot read the keys file") != std::string::npos);

    // Garbage file.
    const std::string garbage = temp_path("garbage.keys");
    write_text(garbage, "hello world\nnot a key line\nname = nothex\n");
    msg.clear();
    CHECK(throws_error([&] { KeySet k; k.load_file(garbage); }, Fail::Keys, msg));
    CHECK(msg.find("does not look like a keys file") != std::string::npos);

    // Empty file.
    const std::string empty = temp_path("empty.keys");
    write_text(empty, "");
    msg.clear();
    CHECK(throws_error([&] { KeySet k; k.load_file(empty); }, Fail::Keys, msg));

    std::filesystem::remove(widen(garbage));
    std::filesystem::remove(widen(empty));
}

void test_merge() {
    const std::string a = temp_path("merge_a.keys");
    const std::string b = temp_path("merge_b.keys");
    const Synthetic s;
    write_text(a, "master_key_00 = " + lo(s.mk00) + "\ntitlekek_00 = " + lo(pattern(20)) + "\n");
    write_text(b, "titlekek_00 = " + lo(pattern(21)) + "\naes_kek_generation_source = " + lo(s.aes_kek_src) + "\n");

    KeySet ks;
    ks.load_file(a);
    ks.load_file(b);
    std::filesystem::remove(widen(a));
    std::filesystem::remove(widen(b));

    CHECK_EQ(ks.count(), static_cast<size_t>(3));
    CHECK(ks.has("master_key_00"));                        // from the first file
    CHECK(ks.has("aes_kek_generation_source"));            // from the second file
    CHECK(ks.titlekek(0) == pattern(21));                  // second file overrides the first
}

void test_derivation() {
    const Synthetic s;
    const Expected e = expected_values(s);

    // Sources and master keys only: everything is derived.
    const std::string src_file = temp_path("derive_sources.keys");
    write_text(src_file, synthetic_text(s));
    KeySet derived;
    derived.load_file(src_file);
    std::filesystem::remove(widen(src_file));

    CHECK(derived.key_area_key(0, 0x0a) == e.kaak_app_0a);
    CHECK(derived.titlekek(0x0a) == e.titlekek_0a);
    CHECK(derived.header_key() == e.header);
    CHECK_EQ(derived.describe(), std::string("8 keys, master keys 00..0a, 0 title keys"));

    // Pre-derived values present: they are used as they are.
    const std::string pre_file = temp_path("derive_prederived.keys");
    write_text(pre_file, "key_area_key_application_0a = " + lo(e.kaak_app_0a) + "\ntitlekek_0a = " +
                             lo(e.titlekek_0a) + "\nheader_key = " + hex_encode(e.header.data(), 32) + "\n");
    KeySet pre;
    pre.load_file(pre_file);
    std::filesystem::remove(widen(pre_file));
    CHECK(pre.key_area_key(0, 0x0a) == e.kaak_app_0a);
    CHECK(pre.titlekek(0x0a) == e.titlekek_0a);
    CHECK(pre.header_key() == e.header);

    // An explicit entry wins over a derivation: titlekek_0a set to something else is returned as-is.
    const std::string explicit_file = temp_path("derive_explicit.keys");
    write_text(explicit_file, synthetic_text(s) + "titlekek_0a = " + lo(pattern(30)) + "\n");
    KeySet explicit_ks;
    explicit_ks.load_file(explicit_file);
    std::filesystem::remove(widen(explicit_file));
    CHECK(explicit_ks.titlekek(0x0a) == pattern(30));
    CHECK(explicit_ks.titlekek(0x0a) != e.titlekek_0a);
}

void test_missing_keys() {
    const Synthetic s;
    const std::vector<std::string> secrets = all_secret_hex(s);

    // Master key 0x0b is not in the file: derivations for generation 0x0b must fail naming it, without key values.
    const std::string file = temp_path("missing_mk.keys");
    write_text(file, synthetic_text(s));
    KeySet ks;
    ks.load_file(file);
    std::filesystem::remove(widen(file));

    std::string msg;
    CHECK(throws_error([&] { (void)ks.titlekek(0x0b); }, Fail::Keys, msg));
    CHECK(msg.find("master_key_0b") != std::string::npos);
    for (const std::string& h : secrets) CHECK(msg.find(h) == std::string::npos);

    msg.clear();
    CHECK(throws_error([&] { (void)ks.key_area_key(0, 0x0b); }, Fail::Keys, msg));
    CHECK(msg.find("master_key_0b") != std::string::npos);
    for (const std::string& h : secrets) CHECK(msg.find(h) == std::string::npos);

    // header_key needs master_key_00: a file with sources but no master keys names it.
    const std::string no_master = temp_path("no_master.keys");
    write_text(no_master, synthetic_text(s, "master_key_00") + "master_key_0a = " + lo(s.mk0a) + "\n");
    KeySet nm;
    nm.load_file(no_master);
    std::filesystem::remove(widen(no_master));
    msg.clear();
    CHECK(throws_error([&] { (void)nm.header_key(); }, Fail::Keys, msg));
    CHECK(msg.find("master_key_00") != std::string::npos);

    // A missing source is named in the error.
    const std::string no_src = temp_path("no_source.keys");
    write_text(no_src, synthetic_text(s, "aes_kek_generation_source"));
    KeySet ns;
    ns.load_file(no_src);
    std::filesystem::remove(widen(no_src));
    msg.clear();
    CHECK(throws_error([&] { (void)ns.key_area_key(0, 0x0a); }, Fail::Keys, msg));
    CHECK(msg.find("aes_kek_generation_source") != std::string::npos);
    for (const std::string& h : secrets) CHECK(msg.find(h) == std::string::npos);
}

std::optional<Key128> try_kaak(const KeySet& ks, int g) {
    try {
        return ks.key_area_key(0, g);
    } catch (const Error&) {
        return std::nullopt;
    }
}

std::optional<Key128> try_titlekek(const KeySet& ks, int g) {
    try {
        return ks.titlekek(g);
    } catch (const Error&) {
        return std::nullopt;
    }
}

// The derivation-only copy of the real file: its sources and master keys, nothing pre-derived.
std::string derive_only_text(const std::string& real_text) {
    static const std::vector<std::string> wanted = {
        "aes_kek_generation_source",         "aes_key_generation_source", "titlekek_source",
        "key_area_key_application_source",   "header_kek_source",         "header_key_source",
    };
    std::string out;
    size_t pos = 0;
    while (pos < real_text.size()) {
        size_t nl = real_text.find('\n', pos);
        if (nl == std::string::npos) nl = real_text.size();
        std::string line = real_text.substr(pos, nl - pos);
        pos = nl + 1;
        const auto strip = [](std::string s) {
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
            size_t b = 0;
            while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
            return s.substr(b);
        };
        line = strip(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = strip(line.substr(0, eq));
        for (char& c : name) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        const std::string value = strip(line.substr(eq + 1));
        const bool is_master = name.rfind("master_key_", 0) == 0;
        bool is_source = false;
        for (const std::string& w : wanted) is_source = is_source || name == w;
        if (is_master || is_source) out += name + " = " + value + "\n";
    }
    return out;
}

void test_real_data() {
    if (!std::filesystem::exists(std::filesystem::path(widen(kRealProd)))) {
        std::printf("real data: SKIPPED, %s does not exist\n", kRealProd);
        return;
    }

    KeySet real;
    real.load_file(kRealProd);
    std::printf("real prod.keys: %s\n", real.describe().c_str());

    std::string msg;
    bool real_ok = true;
    try {
        (void)real.header_key();
        (void)real.key_area_key(0, 0x0a);
        (void)real.titlekek(0x0a);
    } catch (const std::exception& e) {
        std::printf("real prod.keys derivation threw: %s\n", e.what());
        real_ok = false;
    }
    CHECK(real_ok);

    // Derive everything again from the sources and master keys only, and compare with the file's own values.
    const std::string derive_path = temp_path("real_derive_only.keys");
    write_text(derive_path, derive_only_text(read_all(kRealProd)));
    KeySet derived;
    derived.load_file(derive_path);
    std::filesystem::remove(widen(derive_path));

    const std::array<u8, 32> hr = real.header_key();
    const std::array<u8, 32> hd = derived.header_key();
    std::printf("header_key: %s\n", hr == hd ? "match" : "MISMATCH");
    CHECK(hr == hd);

    for (int g : {0x00, 0x05, 0x0a, 0x10, 0x15}) {
        const auto r = try_kaak(real, g);
        const auto d = try_kaak(derived, g);
        if (!r || !d) {
            std::printf("key_area_key(0,%02x): unavailable\n", g);
            CHECK(false);
        } else {
            std::printf("key_area_key(0,%02x): %s\n", g, *r == *d ? "match" : "MISMATCH");
            CHECK(*r == *d);
        }
        const auto rt = try_titlekek(real, g);
        const auto dt = try_titlekek(derived, g);
        if (!rt || !dt) {
            std::printf("titlekek(%02x): unavailable\n", g);
            CHECK(false);
        } else {
            std::printf("titlekek(%02x): %s\n", g, *rt == *dt ? "match" : "MISMATCH");
            CHECK(*rt == *dt);
        }
    }

    // title.keys
    if (std::filesystem::exists(std::filesystem::path(widen(kRealTitle)))) {
        KeySet titles;
        titles.load_file(kRealTitle);
        std::printf("real title.keys: %s\n", titles.describe().c_str());
        CHECK_EQ(titles.title_key_count(), static_cast<size_t>(19));
        std::array<u8, 16> rid{};
        CHECK(hex_decode("010012101468c000000000000000000b", rid.data(), rid.size()));
        Key128 tk{};
        const bool found = titles.title_key(rid, tk);
        // this dump carries its ticket in the NSP, so its rights id is not expected in title.keys
        std::printf("rights id 010012101468c000000000000000000b: %s (not required)\n", found ? "found" : "not found");
        // A rights id that is in this title.keys (its name is listed in the file) proves the lookup itself.
        std::array<u8, 16> rid2{};
        CHECK(hex_decode("01006a800016e8000000000000000007", rid2.data(), rid2.size()));
        Key128 tk2{};
        const bool found2 = titles.title_key(rid2, tk2);
        std::printf("rights id 01006a800016e8000000000000000007: %s\n", found2 ? "found" : "not found");
        CHECK(found2);
    } else {
        std::printf("title.keys: SKIPPED, %s does not exist\n", kRealTitle);
    }
}

void test_find_key_files() {
    const std::vector<KeyFileCandidate> found = find_key_files();
    std::printf("find_key_files: %zu candidate(s)\n", found.size());
    for (const KeyFileCandidate& c : found) std::printf("  source: %s\n", c.source.c_str());

    CHECK(sibling_title_keys("D:\\nonexistent_folder_for_test\\prod.keys").empty());
    if (std::filesystem::exists(std::filesystem::path(widen(kRealProd)))) {
        CHECK(!sibling_title_keys(kRealProd).empty());
    }
}

}  // namespace

int main() {
    test_parser();
    test_merge();
    test_derivation();
    test_missing_keys();
    test_real_data();
    test_find_key_files();
    return TEST_SUMMARY("test_keys");
}
