// SPDX-License-Identifier: GPL-3.0-or-later
// Proof for the PACK reader (pak.h) and the TXTR asset (txtr.h). Four steps:
//  1. every *.pak under the RomFS opens; entries are counted by type; TXTR entries whose stored size differs from the
//     decompressed size are counted (expected 0)
//  2. every TXTR header (kind, format, size, mips, layers, gpu_size, tile mode, swizzle) equals the reference table row of the
//     same asset id (work/survey/textures/txtr_all.tsv)
//  3. the tiled GPU buffer of each id listed in MPRS_TXTR_REF has the SHA-256 the Python reference (scripts/txtr_ref_hashes.py)
//     gives; then every TXTR entry is decoded once on 8 threads
//  4. synthetic checks that need no game data: LZSS modes, malformed streams, the format table, UUID text, SHA-256 itself
// Environment: MPRS_ROMFS (default D:\AI\Active\Metroid-Prime\game\romfs), MPRS_TXTR_TSV (default ...\work\survey\textures\
// txtr_all.tsv), MPRS_TXTR_REF (the file scripts/txtr_ref_hashes.py writes). Without the romfs, steps 1-3 are skipped with a note.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "pak.h"
#include "txtr.h"

using namespace mprs;
namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<u8>;

// Same names and order as KINDS in tools/survey/txtr_tally.py (TxtrKind is the index).
const char* const kKindNames[] = {"1D", "2D", "3D", "Cube", "1DArray", "2DArray", "2DMS", "2DMSArray", "CubeArray"};

void record_failure(const std::string& msg) {
    ++mprs_test::checks();
    ++mprs_test::failures();
    std::printf("FAIL %s\n", msg.c_str());
}

// ---- SHA-256, used only here to compare decoded buffers with the Python reference
struct Sha256 {
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u8 buf[64] = {};
    size_t blen = 0;
    u64 total = 0;

    static u32 rotr(u32 x, unsigned n) { return (x >> n) | (x << (32 - n)); }

    void compress(const u8* p) {
        static const u32 K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        u32 w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = u32(p[4 * i]) << 24 | u32(p[4 * i + 1]) << 16 | u32(p[4 * i + 2]) << 8 | u32(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const u32 S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const u32 ch = (e & f) ^ (~e & g);
            const u32 t1 = hh + S1 + ch + K[i] + w[i];
            const u32 S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const u32 maj = (a & b) ^ (a & c) ^ (b & c);
            const u32 t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    void update(const u8* p, size_t n) {
        total += n;
        while (n) {
            const size_t take = std::min(n, 64 - blen);
            std::memcpy(buf + blen, p, take);
            blen += take;
            p += take;
            n -= take;
            if (blen == 64) {
                compress(buf);
                blen = 0;
            }
        }
    }
    std::string hex() {
        const u64 bits = total * 8;
        const u8 pad = 0x80, zero = 0;
        update(&pad, 1);
        while (blen != 56) update(&zero, 1);
        u8 len[8];
        for (int i = 0; i < 8; ++i) len[i] = u8(bits >> (56 - 8 * i));
        update(len, 8);
        std::string s;
        char tmp[3];
        for (int i = 0; i < 8; ++i)
            for (int k = 3; k >= 0; --k) {
                std::snprintf(tmp, sizeof tmp, "%02x", (h[i] >> (8 * k)) & 0xff);
                s += tmp;
            }
        return s;
    }
};

std::string sha256_hex(const Bytes& d) {
    Sha256 s;
    s.update(d.data(), d.size());
    return s.hex();
}

template <class F>
bool throws_input(F&& f) {
    try {
        f();
    } catch (const Error& e) {
        return e.kind == Fail::Input;
    } catch (...) {
    }
    return false;
}

Bytes dec(const Bytes& stream, size_t out_len) { return txtr_decompress(stream.data(), stream.size(), out_len); }

Bytes text(const char* s) { return Bytes(s, s + std::strlen(s)); }

// ---- step 4: synthetic checks, no game data
void synthetic_tests() {
    // mode 0 is stored data
    CHECK(dec({0, 0, 0, 0, 1, 2, 3}, 3) == Bytes({1, 2, 3}));
    CHECK(throws_input([] { (void)dec({0, 0, 0, 0, 1, 2, 3}, 4); }));  // stored length differs from the expected size

    // mode 1: flags 0x40 = literal, back-reference. Back-reference b0 = 0x20 -> count 5, b1 = 0x01 -> length 1 (overlapping)
    CHECK(dec({1, 0, 0, 0, 0x40, 'x', 0x20, 0x01}, 6) == text("xxxxxx"));
    // flags 0x10 = literal, literal, literal, back-reference; b0 = 0x00 -> count 3, length 3 (not overlapping)
    CHECK(dec({1, 0, 0, 0, 0x10, 'a', 'b', 'c', 0x00, 0x03}, 6) == text("abcabc"));

    // mode 2: groups of 2 bytes. Back-reference count (0+2)=2 groups = 4 bytes, length 1 group = 2 bytes (overlapping)
    CHECK(dec({2, 0, 0, 0, 0x40, 'a', 'b', 0x00, 0x01}, 6) == text("ababab"));

    // mode 3: groups of 4 bytes. Non-overlapping: count 1 group, length 4 bytes
    CHECK(dec({3, 0, 0, 0, 0x40, 'A', 'B', 'C', 'D', 0x00, 0x01}, 8) == text("ABCDABCD"));
    // overlapping: b0 = 0x10 -> count 2 groups = 8 bytes, length 4 bytes
    CHECK(dec({3, 0, 0, 0, 0x40, 'A', 'B', 'C', 'D', 0x10, 0x01}, 12) == text("ABCDABCDABCD"));

    // malformed streams must throw Error(Input)
    CHECK(throws_input([] { (void)dec({1, 0, 0, 0, 0x40, 'x', 0x20}, 6); }));           // back-reference cut short
    CHECK(throws_input([] { (void)dec({1, 0, 0, 0, 0x00}, 1); }));                       // literal with no data
    CHECK(throws_input([] { (void)dec({1, 0, 0, 0, 0x00, 'a'}, 5); }));                  // output shorter than declared
    CHECK(throws_input([] { (void)dec({1, 0, 0, 0, 0x40, 'x', 0x20, 0x01}, 5); }));      // output would be longer than declared
    CHECK(throws_input([] { (void)dec({1, 0, 0, 0, 0x80, 0x00, 0x01}, 1); }));           // reference before start of output
    CHECK(throws_input([] { (void)dec({1, 0, 0, 0, 0x40, 'x', 0x00, 0x00}, 2); }));      // back-reference of length zero
    CHECK(throws_input([] { (void)dec({4, 0, 0, 0, 1, 2}, 2); }));                       // unknown mode
    CHECK(throws_input([] { (void)dec({1, 0}, 1); }));                                   // shorter than the mode word

    // UUID text, bytes 00..0f stored little-endian
    Uuid u;
    for (int i = 0; i < 16; ++i) u.b[i] = static_cast<u8>(i);
    CHECK(u.str() == "03020100-0504-0706-0809-0a0b0c0d0e0f");

    // format table: index 52 is "None" (0 bytes), BC1 is 8 bytes per 4x4, BptcUnormSrgb is sRGB 16 bytes
    CHECK(txtr_format(52).bytes == 0);
    CHECK(std::strcmp(txtr_format(0).name, "R8Unorm") == 0);
    CHECK(std::strcmp(txtr_format(53).name, "Astc4x4") == 0);
    CHECK(txtr_format(20).bytes == 8 && txtr_format(20).bc && txtr_format(20).block_w == 4);
    CHECK(txtr_format(84).srgb && txtr_format(84).bytes == 16);
    CHECK(throws_input([] { (void)txtr_format(85); }));

    // SHA-256 of "abc" (FIPS 180 test vector), used to vouch for the hasher in step 3
    CHECK(sha256_hex(text("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// ---- helpers for the game-data steps
std::vector<std::string> find_paks(const fs::path& root) {
    std::vector<std::string> out;
    for (const auto& de : fs::recursive_directory_iterator(root)) {
        if (de.is_regular_file() && de.path().extension() == ".pak") out.push_back(narrow(de.path().wstring()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct TableRow {
    std::string path, kind, fmt;
    u64 w = 0, h = 0, layers = 0, mips = 0, tile_mode = 0, swizzle = 0, gpu_size = 0;
};

// Reads the survey table by column name. Returns false (after recording the failure) if the file cannot be read.
bool read_table(const std::string& file, std::map<std::string, std::vector<TableRow>>& by_id, size_t& nrows) {
    std::ifstream in(file);
    if (!in) {
        record_failure("cannot open the reference table " + file);
        return false;
    }
    std::string line;
    if (!std::getline(in, line)) return false;
    std::map<std::string, size_t> col;
    {
        size_t start = 0;
        for (size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == '\t') {
                col[line.substr(start, i - start)] = col.size();
                start = i + 1;
            }
        }
    }
    const char* need[] = {"path", "id", "kind", "fmt", "w", "h", "layers", "mips", "tile_mode", "swizzle", "gpu_size"};
    for (const char* n : need) {
        if (!col.count(n)) {
            record_failure(std::string("the reference table has no column ") + n);
            return false;
        }
    }
    nrows = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> f;
        size_t start = 0;
        for (size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == '\t') {
                f.push_back(line.substr(start, i - start));
                start = i + 1;
            }
        }
        auto at = [&](const char* name) -> const std::string& { return f.at(col[name]); };
        TableRow r;
        try {
            r.path = at("path");
            r.kind = at("kind");
            r.fmt = at("fmt");
            r.w = std::stoull(at("w"));
            r.h = std::stoull(at("h"));
            r.layers = std::stoull(at("layers"));
            r.mips = std::stoull(at("mips"));
            r.tile_mode = std::stoull(at("tile_mode"));
            r.swizzle = std::stoull(at("swizzle"));
            r.gpu_size = std::stoull(at("gpu_size"));
        } catch (const std::exception& ex) {
            record_failure("bad row in the reference table (" + std::string(ex.what()) + "): " + line.substr(0, 80));
            continue;
        }
        by_id[at("id")].push_back(r);
        ++nrows;
    }
    return true;
}

struct TxtrRef {
    const Pak* pak;
    PakEntry e;
    std::string id;
};

}  // namespace

int main() {
    synthetic_tests();

    const char* env_romfs = std::getenv("MPRS_ROMFS");
    const fs::path romfs = (env_romfs && *env_romfs) ? fs::path(env_romfs) : fs::path(R"(D:\AI\Active\Metroid-Prime\game\romfs)");
    if (!fs::is_directory(romfs)) {
        std::printf("SKIP: the game romfs was not found at \"%s\" (set MPRS_ROMFS). Steps 1-3 were not run.\n",
                    romfs.string().c_str());
        return TEST_SUMMARY("pak");
    }

    // ---- step 1: open every pak, count entries
    std::vector<std::string> paths;
    try {
        paths = find_paks(romfs);
    } catch (const std::exception& ex) {
        record_failure(std::string("cannot list the romfs: ") + ex.what());
        return TEST_SUMMARY("pak");
    }
    std::vector<std::unique_ptr<Pak>> paks;
    std::vector<std::string> pak_paths;
    std::map<std::string, size_t> by_type;
    size_t total_entries = 0, txtr_size_mismatch = 0;
    std::vector<TxtrRef> txtr;
    u64 pak_bytes = 0;
    for (const auto& p : paths) {
        try {
            auto pak = Pak::open(open_file_storage(p));
            pak_bytes += pak->file()->size();
            for (const auto& e : pak->assets()) {
                ++by_type[e.type];
                ++total_entries;
                if (std::strcmp(e.type, "TXTR") == 0) {
                    if (e.size != e.decompressed_size) ++txtr_size_mismatch;
                    txtr.push_back({pak.get(), e, e.id.str()});
                }
            }
            paks.push_back(std::move(pak));
            pak_paths.push_back(p);
        } catch (const std::exception& ex) {
            record_failure("cannot open pak " + p + ": " + ex.what());
        }
    }
    ++mprs_test::checks();
    std::printf("step 1: %zu paks opened (of %zu found, %.1f MB), %zu entries\n", paks.size(), paths.size(), pak_bytes / 1e6,
                total_entries);
    for (const auto& [t, n] : by_type) std::printf("  %-6s %zu\n", t.c_str(), n);
    std::printf("  TXTR entries: %zu, of which size != decompressed_size: %zu\n", txtr.size(), txtr_size_mismatch);
    ++mprs_test::checks();
    if (txtr_size_mismatch != 0) {
        ++mprs_test::failures();
        std::printf("FAIL %zu TXTR entries are stored compressed (size != decompressed_size)\n", txtr_size_mismatch);
    }

    // ---- step 2: TXTR headers against the reference table
    const char* env_tsv = std::getenv("MPRS_TXTR_TSV");
    const std::string tsv = (env_tsv && *env_tsv) ? env_tsv : R"(D:\AI\Active\Metroid-Prime\work\survey\textures\txtr_all.tsv)";
    std::map<std::string, std::vector<TableRow>> table;
    size_t table_rows = 0;
    read_table(tsv, table, table_rows);
    std::set<std::string> found_ids;
    size_t header_mismatch = 0, no_row = 0, read_errors = 0;
    for (const auto& r : txtr) {
        TxtrInfo info;
        try {
            info = txtr_read_info(*r.pak, r.e);
        } catch (const std::exception& ex) {
            ++read_errors;
            if (read_errors <= 10) record_failure("txtr_read_info " + r.id + ": " + ex.what());
            continue;
        }
        const auto it = table.find(r.id);
        if (it == table.end()) {
            ++no_row;   // not surveyed (the survey could not extract SamusGunMP1.pak); decoded in step 3b instead
            continue;
        }
        found_ids.insert(r.id);
        bool ok = true;
        for (const auto& row : it->second) {
            const std::string kind = static_cast<size_t>(info.kind) < 9 ? kKindNames[static_cast<size_t>(info.kind)] : "?";
            const std::string fmt = txtr_format(info.format).name;
            const bool same = kind == row.kind && fmt == row.fmt && info.width == row.w && info.height == row.h &&
                              info.layers == row.layers && info.mips == row.mips && info.tile_mode == row.tile_mode &&
                              info.swizzle == row.swizzle && info.gpu_size == row.gpu_size;
            if (!same) {
                ok = false;
                if (header_mismatch < 10)
                    record_failure("header of " + r.id + ": got " + kind + " " + fmt + " " + std::to_string(info.width) + "x" +
                                   std::to_string(info.height) + " layers " + std::to_string(info.layers) + " mips " +
                                   std::to_string(info.mips) + " gpu " + std::to_string(info.gpu_size) + "; table row " +
                                   row.kind + " " + row.fmt + " " + std::to_string(row.w) + "x" + std::to_string(row.h) +
                                   " layers " + std::to_string(row.layers) + " mips " + std::to_string(row.mips) + " gpu " +
                                   std::to_string(row.gpu_size));
            }
        }
        if (!ok) ++header_mismatch;
    }
    std::set<std::string> table_ids;
    for (const auto& [id, rows] : table) table_ids.insert(id);
    size_t missing_from_paks = 0;
    for (const auto& id : table_ids)
        if (!found_ids.count(id)) ++missing_from_paks;
    std::printf("step 2: %zu TXTR entries checked against %zu table rows (%zu distinct ids)\n", txtr.size(), table_rows,
                table_ids.size());
    std::printf("  header mismatches: %zu (of which not in the table: %zu), read errors: %zu\n", header_mismatch, no_row,
                read_errors);
    std::printf("  table ids not found in the paks: %zu\n", missing_from_paks);
    ++mprs_test::checks();
    if (header_mismatch != 0 || read_errors != 0) {
        ++mprs_test::failures();
        std::printf("FAIL header check: %zu mismatches, %zu read errors\n", header_mismatch, read_errors);
    }
    ++mprs_test::checks();
    if (missing_from_paks != 0) {
        ++mprs_test::failures();
        std::printf("FAIL %zu table ids were not found in any pak\n", missing_from_paks);
    }

    // ---- step 3a: decoded buffers against the Python reference hashes
    std::map<std::string, std::vector<size_t>> refs_by_id;
    for (size_t i = 0; i < txtr.size(); ++i) refs_by_id[txtr[i].id].push_back(i);
    const char* env_ref = std::getenv("MPRS_TXTR_REF");
    if (!env_ref || !*env_ref) {
        std::printf("NOTE: MPRS_TXTR_REF not set, python-reference hash check skipped (scripts/txtr_ref_hashes.py writes the file)\n");
    } else {
        std::ifstream in(env_ref);
        if (!in) {
            record_failure(std::string("cannot open the reference hashes ") + env_ref);
        } else {
            size_t lines = 0, hashed = 0, matches = 0, mism = 0;
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                const size_t tab = line.find('\t');
                if (tab == std::string::npos || line.size() - tab - 1 != 64) {
                    record_failure("bad line in the reference hashes: " + line.substr(0, 80));
                    continue;
                }
                ++lines;
                const std::string id = line.substr(0, tab), want = line.substr(tab + 1);
                const auto it = refs_by_id.find(id);
                if (it == refs_by_id.end()) {
                    ++mism;
                    record_failure("reference id " + id + " has no TXTR entry in the paks");
                    continue;
                }
                for (size_t idx : it->second) {
                    const TxtrRef& r = txtr[idx];
                    try {
                        const TxtrInfo info = txtr_read_info(*r.pak, r.e);
                        const Bytes tiled = txtr_read_tiled(*r.pak, r.e, info);
                        ++hashed;
                        if (tiled.size() != info.gpu_size) {
                            ++mism;
                            record_failure("decoded size of " + id + " differs from gpu_size");
                        } else if (sha256_hex(tiled) == want) {
                            ++matches;
                        } else {
                            ++mism;
                            record_failure("tiled buffer of " + id + " does not match the reference SHA-256");
                        }
                    } catch (const std::exception& ex) {
                        ++mism;
                        record_failure("txtr_read_tiled " + id + ": " + ex.what());
                    }
                }
            }
            std::printf("step 3a: %zu reference ids, %zu asset entries decoded, %zu SHA-256 matches, %zu mismatches\n", lines,
                        hashed, matches, mism);
            ++mprs_test::checks();
            if (lines == 0 || mism != 0) {
                ++mprs_test::failures();
                std::printf("FAIL reference hashes: %zu lines, %zu mismatches\n", lines, mism);
            }
        }
    }

    // ---- step 3b: decode every TXTR entry once, 8 threads
    const unsigned kThreads = 8;
    std::atomic<size_t> next{0}, failed{0};
    std::atomic<u64> bytes_out{0};
    std::mutex mu;
    std::vector<std::string> errors;
    auto worker = [&]() {
        for (;;) {
            const size_t k = next.fetch_add(1);
            if (k >= txtr.size()) return;
            const TxtrRef& r = txtr[k];
            try {
                const TxtrInfo info = txtr_read_info(*r.pak, r.e);
                const Bytes buf = txtr_read_tiled(*r.pak, r.e, info);
                if (buf.size() != info.gpu_size) throw std::runtime_error("the decoded size differs from gpu_size");
                bytes_out += buf.size();
            } catch (const std::exception& ex) {
                ++failed;
                std::lock_guard<std::mutex> lk(mu);
                if (errors.size() < 10) errors.push_back(r.id + ": " + ex.what());
            }
        }
    };
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < kThreads; ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("step 3b: decoded %zu TXTR entries on %u threads: %zu failures, %.1f MB of GPU data, %.2f s\n", txtr.size(),
                kThreads, failed.load(), bytes_out.load() / 1e6, secs);
    for (const auto& m : errors) std::printf("  %s\n", m.c_str());
    ++mprs_test::checks();
    if (failed.load() != 0) {
        ++mprs_test::failures();
        std::printf("FAIL %zu TXTR entries failed to decode\n", failed.load());
    }

    return TEST_SUMMARY("pak");
}
