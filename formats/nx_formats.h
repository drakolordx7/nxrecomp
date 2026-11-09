// SPDX-License-Identifier: GPL-3.0-or-later
// The file formats the runtime and the setup tool both read or write, defined once. Header only; the Python twin is nx_formats.py (same
// constants and the same functions, checked against this file by test_formats.py and by tools/setup/tests/test_formats.cpp).
//
//   1. the hash that keys the BC7 texture cache
//   2. the BC7 cache file: key, header, level table, path
//   3. programs.bin, the shader program list the setup tool scans from the player's game files and the runtime compiles ahead of play
//   4. the shader pack, the one file a release ships its translated shaders in (the setup tool unpacks it)
//   5. setup_state.tsv, what the setup tool finished (written by the setup tool, read by the launcher)
//   6. shader_cache/prepare.state, what a shader preparation run finished (written and read by the game exe, read by the launcher)
//   7. the "prepare:" stdout lines of `<game exe> --prepare-shaders` (printed by the game exe, parsed by the launcher)
//
// Magics: the neutral ones (NXPRG002, NXSHC002, nx_setup_state) are what is written now. The magics the first Metroid release wrote
// (MPRPRG01, MPRSHC01, mprs_setup_state) have the same layout and are still accepted by every reader, so an existing install keeps working.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace nxfmt {

// ---- 1. hash ----------------------------------------------------------------------------------------------------------------
// xxHash64-style mix: four lanes over 32-byte stripes, then a final avalanche. Not byte compatible with xxHash; it only has to be fast and
// to separate textures. The cache also compares its header, so a collision cannot return wrong data silently.
namespace detail {
constexpr uint64_t kP1 = 0x9E3779B185EBCA87ull, kP2 = 0xC2B2AE3D27D4EB4Full, kP3 = 0x165667B19E3779F9ull, kP4 = 0x85EBCA77C2B2AE63ull,
                   kP5 = 0x27D4EB2F165667C5ull;
inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t read64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint32_t read32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t lane(uint64_t acc, uint64_t in) { acc += in * kP2; acc = rotl64(acc, 31); return acc * kP1; }
inline uint64_t merge_lane(uint64_t h, uint64_t v) { h ^= lane(0, v); return h * kP1 + kP4; }
}  // namespace detail

inline uint64_t hash_seeded(const void* data, size_t n, uint64_t seed) {
    using namespace detail;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* const end = p + n;
    uint64_t h;
    if (n >= 32) {
        uint64_t v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
        do {
            v1 = lane(v1, read64(p));
            v2 = lane(v2, read64(p + 8));
            v3 = lane(v3, read64(p + 16));
            v4 = lane(v4, read64(p + 24));
            p += 32;
        } while (end - p >= 32);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = merge_lane(h, v1);
        h = merge_lane(h, v2);
        h = merge_lane(h, v3);
        h = merge_lane(h, v4);
    } else {
        h = seed + kP5;
    }
    h += n;
    while (end - p >= 8) {
        h ^= lane(0, read64(p));
        h = rotl64(h, 27) * kP1 + kP4;
        p += 8;
    }
    if (end - p >= 4) {
        h ^= static_cast<uint64_t>(read32(p)) * kP1;
        h = rotl64(h, 23) * kP2 + kP3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<uint64_t>(*p) * kP5;
        h = rotl64(h, 11) * kP1;
        ++p;
    }
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}
inline uint64_t hash_bytes(const void* data, size_t n) { return hash_seeded(data, n, 0); }

// Little-endian helpers for the file formats below.
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
inline void put64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
inline uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t get64(const uint8_t* p) { return static_cast<uint64_t>(get32(p)) | (static_cast<uint64_t>(get32(p + 4)) << 32); }

// ---- 2. BC7 texture cache -----------------------------------------------------------------------------------------------------
// One file per texture, <cache dir>/<first two hex digits of the key>/<16 hex digits, lower case>.bc7:
//   magic "NXB7", u32 version, u64 key, u32 VkFormat of the data, u32 level count, u32 layer count,
//   then per level {u32 width, u32 height, u32 depth, u64 offset, u64 size} (offset into the data block), then the data.
constexpr uint32_t kBc7CacheVersion = 1;
constexpr uint8_t kBc7CacheMagic[4] = {'N', 'X', 'B', '7'};
constexpr size_t kBc7HeadBytes = 4 + 4 + 8 + 4 + 4 + 4;   // magic, version, key, VkFormat, level count, layer count
constexpr size_t kBc7LevelBytes = 3 * 4 + 2 * 8;          // per level: width, height, depth, offset, size

struct Bc7Level {
    uint32_t width = 0, height = 0, depth = 0;
    uint64_t offset = 0, size = 0;
};

// What decides the output besides the source bytes: the NVN format enum value of the texture, the level-0 size, the level and layer
// counts, the block height (GOBs) of level 0, the pitch (0 for block-linear) and the BC7 preset.
struct Bc7Meta {
    uint64_t nvn_format = 0, width = 0, height = 0, depth = 0, levels = 0, layers = 0, block_height = 0, pitch = 0, quality = 0;
};

// The key of a texture whose game memory is `mem` of `storage_size` bytes: hash_seeded(mem, storage_size, seed = hash_bytes(meta)), with
// meta = {kBc7CacheVersion, then the Bc7Meta fields in the order above}: ten u64.
inline uint64_t bc7_cache_key(const void* mem, size_t storage_size, const Bc7Meta& m) {
    const uint64_t meta[10] = {kBc7CacheVersion, m.nvn_format, m.width, m.height, m.depth, m.levels, m.layers, m.block_height, m.pitch,
                               m.quality};
    return hash_seeded(mem, storage_size, hash_bytes(meta, sizeof meta));
}

// "<2 hex digits>/<16 hex digits>.bc7", '/' separated.
inline std::string bc7_cache_rel_path(uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(key));
    return std::string(name, 2) + "/" + name + ".bc7";
}

inline void bc7_put_head(std::vector<uint8_t>& out, uint64_t key, uint32_t vk_format, uint32_t layers, const Bc7Level* levels, size_t count) {
    out.insert(out.end(), kBc7CacheMagic, kBc7CacheMagic + 4);
    put32(out, kBc7CacheVersion);
    put64(out, key);
    put32(out, vk_format);
    put32(out, static_cast<uint32_t>(count));
    put32(out, layers);
    for (size_t i = 0; i < count; ++i) {
        put32(out, levels[i].width);
        put32(out, levels[i].height);
        put32(out, levels[i].depth);
        put64(out, levels[i].offset);
        put64(out, levels[i].size);
    }
}
// True when the first kBc7HeadBytes at p (n bytes available) carry the magic, this version and `key`.
inline bool bc7_head_matches(const uint8_t* p, size_t n, uint64_t key) {
    return n >= kBc7HeadBytes && std::memcmp(p, kBc7CacheMagic, 4) == 0 && get32(p + 4) == kBc7CacheVersion && get64(p + 8) == key;
}

// ---- 3. programs.bin ----------------------------------------------------------------------------------------------------------
// <shader cache folder>/../programs.bin: magic (8), u32 count, u32 0, then count records of three u64 (vs, fs, gs; gs = 0 when the
// program has no geometry stage), little-endian, strictly ascending by (vs, fs, gs). A stage hash is the first 8 bytes of the SHA-1 of
// its code block read as a big-endian u64: the first 16 hex digits of the shader cache file name.
constexpr char kProgramsMagic[8] = {'N', 'X', 'P', 'R', 'G', '0', '0', '2'};
constexpr char kProgramsMagicLegacy[8] = {'M', 'P', 'R', 'P', 'R', 'G', '0', '1'};   // the first Metroid release; same layout
constexpr size_t kProgramsHeadBytes = 16;
constexpr size_t kProgramsRecordBytes = 24;

struct ProgramHashes {
    uint64_t vs = 0, fs = 0, gs = 0;
    bool operator==(const ProgramHashes& o) const { return vs == o.vs && fs == o.fs && gs == o.gs; }
    bool operator<(const ProgramHashes& o) const { return vs != o.vs ? vs < o.vs : fs != o.fs ? fs < o.fs : gs < o.gs; }
};

inline bool programs_magic_ok(const uint8_t* p) {
    return std::memcmp(p, kProgramsMagic, 8) == 0 || std::memcmp(p, kProgramsMagicLegacy, 8) == 0;
}

// The bytes of a program list (the programs must be sorted and unique).
inline std::vector<uint8_t> programs_encode(const std::vector<ProgramHashes>& programs) {
    std::vector<uint8_t> b(kProgramsMagic, kProgramsMagic + 8);
    put32(b, static_cast<uint32_t>(programs.size()));
    put32(b, 0);
    for (const ProgramHashes& p : programs) {
        put64(b, p.vs);
        put64(b, p.fs);
        put64(b, p.gs);
    }
    return b;
}

// Decodes a whole file. Returns nullptr when valid, else why not (the size, the magic, the reserved word, the byte count, the order).
inline const char* programs_decode(const uint8_t* data, size_t size, std::vector<ProgramHashes>& out) {
    out.clear();
    if (size < kProgramsHeadBytes) return "the file is too short to be a program list";
    if (!programs_magic_ok(data)) return "the file has the wrong header";
    const uint32_t count = get32(data + 8);
    if (get32(data + 12) != 0) return "the file has a damaged header";
    if (size != kProgramsHeadBytes + kProgramsRecordBytes * static_cast<size_t>(count)) return "the file size does not match its program count";
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = data + kProgramsHeadBytes + kProgramsRecordBytes * static_cast<size_t>(i);
        out[i] = ProgramHashes{get64(p), get64(p + 8), get64(p + 16)};
        if (i > 0 && !(out[i - 1] < out[i])) return "the programs are not sorted";
    }
    return nullptr;
}

// ---- 4. shader pack -----------------------------------------------------------------------------------------------------------
// One file with every translated shader of a release (written by tools/setup/scripts/make_shader_pack.py, unpacked by the setup tool
// into <install>/shader_cache/spirv, the folder the runtime reads: results.tsv and <16 hex digits>.spv):
//   magic (8), u32 entry_count, u32 reserved, then entry_count records
//   {u16 name_len, name bytes, u64 data_offset (from file start), u64 stored_size, u64 raw_size, u8[32] sha256 of the RAW bytes,
//    u8 method (0 = stored, 1 = zstd)}, then the data. Little-endian. A name is "results.tsv" or 16 lower-case hex digits + ".spv".
constexpr char kShaderPackMagic[8] = {'N', 'X', 'S', 'H', 'C', '0', '0', '2'};
constexpr char kShaderPackMagicLegacy[8] = {'M', 'P', 'R', 'S', 'H', 'C', '0', '1'};   // the first Metroid release; same layout
constexpr size_t kShaderPackHeadBytes = 16;
constexpr const char* kShaderResultsName = "results.tsv";

inline bool shader_pack_magic_ok(const void* p) {
    return std::memcmp(p, kShaderPackMagic, 8) == 0 || std::memcmp(p, kShaderPackMagicLegacy, 8) == 0;
}

// ---- 5. setup_state.tsv -------------------------------------------------------------------------------------------------------
// <install>/setup_state.tsv, tab separated, one record per line: the head "nx_setup_state\t1", then "meta\t<key>\t<value>" and
// "file\t<relative path>\t<size>\t<sha256 hex, may be empty>" lines. The last "meta\tfinished\t<0|1>" line says whether a setup run
// ended; unknown lines are ignored. The head the first Metroid release wrote ("mprs_setup_state\t1") is still accepted.
constexpr const char* kSetupStateName = "setup_state.tsv";
constexpr const char* kSetupStateHead = "nx_setup_state\t1";
constexpr const char* kSetupStateHeadLegacy = "mprs_setup_state\t1";

inline std::string setup_state_head_line() { return std::string(kSetupStateHead) + "\n"; }
inline std::string setup_state_meta_line(const std::string& key, const std::string& value) { return "meta\t" + key + "\t" + value + "\n"; }
inline std::string setup_state_file_line(const std::string& rel, uint64_t size, const std::string& sha256) {
    return "file\t" + rel + "\t" + std::to_string(size) + "\t" + sha256 + "\n";
}

// The fields of one line without its end of line (a trailing '\r' is dropped). Empty fields are kept.
inline std::vector<std::string> split_tabs(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    std::vector<std::string> f;
    size_t p = 0;
    for (;;) {
        const size_t q = line.find('\t', p);
        f.emplace_back(line.substr(p, q == std::string_view::npos ? q : q - p));
        if (q == std::string_view::npos) break;
        p = q + 1;
    }
    return f;
}

inline bool setup_state_head_ok(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line == kSetupStateHead || line == kSetupStateHeadLegacy;
}

// True when `text` is a state file whose last "meta finished" record is 1.
inline bool setup_state_finished(std::string_view text) {
    size_t pos = 0;
    bool first = true, finished = false;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string_view::npos) e = text.size();
        const std::string_view line = text.substr(pos, e - pos);
        pos = e + 1;
        if (first) {
            if (!setup_state_head_ok(line)) return false;
            first = false;
            continue;
        }
        const std::vector<std::string> f = split_tabs(line);
        if (f.size() >= 3 && f[0] == "meta" && f[1] == "finished") finished = (f[2] == "1");
    }
    return !first && finished;
}

// ---- 6. shader_cache/prepare.state --------------------------------------------------------------------------------------------
// <game folder>/shader_cache/prepare.state, "name: value" lines in this order: key, programs, prepared, missing, failed, seconds,
// complete (1 = the run finished, 0 = it was cancelled). `key` names the GPU, driver and scheme the run was for; a run is up to date
// when the key matches and complete is 1.
constexpr const char* kPrepareStateName = "prepare.state";

struct PrepareState {
    std::string key;
    uint64_t programs = 0, prepared = 0, missing = 0, failed = 0;
    double seconds = 0;
    bool complete = false;
};

inline std::string prepare_state_encode(const PrepareState& s) {
    std::string t = "key: " + s.key + "\nprograms: " + std::to_string(s.programs) + "\nprepared: " + std::to_string(s.prepared) +
                    "\nmissing: " + std::to_string(s.missing) + "\nfailed: " + std::to_string(s.failed) + "\nseconds: ";
    char b[32];
    std::snprintf(b, sizeof b, "%g", s.seconds);
    return t + b + "\ncomplete: " + (s.complete ? "1" : "0") + "\n";
}

// Reads the lines it knows ("key" and "complete" decide whether a run is up to date; the counters are kept for reports).
inline PrepareState prepare_state_decode(std::string_view text) {
    PrepareState s;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string_view::npos) e = text.size();
        std::string_view l = text.substr(pos, e - pos);
        pos = e + 1;
        while (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        const size_t c = l.find(": ");
        if (c == std::string_view::npos) continue;
        const std::string name(l.substr(0, c)), v(l.substr(c + 2));
        if (name == "key") s.key = v;
        else if (name == "programs") s.programs = std::strtoull(v.c_str(), nullptr, 10);
        else if (name == "prepared") s.prepared = std::strtoull(v.c_str(), nullptr, 10);
        else if (name == "missing") s.missing = std::strtoull(v.c_str(), nullptr, 10);
        else if (name == "failed") s.failed = std::strtoull(v.c_str(), nullptr, 10);
        else if (name == "seconds") s.seconds = std::strtod(v.c_str(), nullptr);
        else if (name == "complete") s.complete = (v == "1");
    }
    return s;
}

// ---- 7. the prepare: output of --prepare-shaders ------------------------------------------------------------------------------
// One line each, on stdout:
//   "prepare: <done> <total>"          progress (programs compiled so far, programs in all)
//   "prepare: ready"                   already up to date, nothing was done
//   "prepare: done programs=N prepared=N missing=N failed=N parts=N fast=N seconds=S[ cancelled]"
//   "prepare: error <text>"            could not run
// The parser takes surrounding blanks and a trailing '\r'; a line that does not fit any of the four is `other`.
enum class PrepareKind { other, progress, ready, done, error };

struct PrepareLine {
    PrepareKind kind = PrepareKind::other;
    unsigned long long done = 0;    // progress: programs compiled so far
    unsigned long long total = 0;   // progress: programs in all
    long long failed = -1;          // done: the failed= count, -1 when the line has none
    bool cancelled = false;         // done: the line ends with "cancelled"
    std::string text;               // error: the error text; ready and done: the rest of the line; other: the whole line
};

struct PrepareDone {
    unsigned long long programs = 0, prepared = 0, missing = 0, failed = 0, parts = 0, fast = 0;
    double seconds = 0;
    bool cancelled = false;
};

inline std::string prepare_line_progress(unsigned long long done, unsigned long long total) {
    return "prepare: " + std::to_string(done) + " " + std::to_string(total) + "\n";
}
inline std::string prepare_line_ready() { return "prepare: ready\n"; }
inline std::string prepare_line_error(const std::string& text) { return "prepare: error " + text + "\n"; }
inline std::string prepare_line_done(const PrepareDone& d) {
    char b[256];
    std::snprintf(b, sizeof b, "prepare: done programs=%llu prepared=%llu missing=%llu failed=%llu parts=%llu fast=%llu seconds=%.1f%s\n", d.programs,
                  d.prepared, d.missing, d.failed, d.parts, d.fast, d.seconds, d.cancelled ? " cancelled" : "");
    return b;
}

namespace detail {
inline std::string trim_blanks(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return std::string(s.substr(a, b - a));
}
inline bool parse_count(const std::string& s, unsigned long long* out) {
    if (s.empty() || s.size() > 18) return false;
    unsigned long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned long long>(c - '0');
    }
    *out = v;
    return true;
}
}  // namespace detail

inline PrepareLine prepare_line_parse(std::string_view raw) {
    using detail::parse_count;
    using detail::trim_blanks;
    PrepareLine out;
    const std::string line = trim_blanks(raw);
    static const std::string kPrefix = "prepare:";
    if (line.compare(0, kPrefix.size(), kPrefix) != 0) {
        out.text = line;
        return out;
    }
    const std::string rest = trim_blanks(line.substr(kPrefix.size()));
    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : rest) {
            if (c == ' ' || c == '\t') {
                if (!cur.empty()) words.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) words.push_back(cur);
    }
    if (words.empty()) {
        out.text = line;
        return out;
    }
    const std::string& w0 = words[0];
    if (w0 == "ready") {
        out.kind = PrepareKind::ready;
        out.text = rest;
        return out;
    }
    if (w0 == "error") {
        out.kind = PrepareKind::error;
        out.text = trim_blanks(rest.substr(w0.size()));
        return out;
    }
    if (w0 == "done") {
        out.kind = PrepareKind::done;
        out.text = rest;
        for (size_t i = 1; i < words.size(); ++i) {
            const std::string& w = words[i];
            if (w.compare(0, 7, "failed=") == 0) {
                unsigned long long n = 0;
                if (parse_count(w.substr(7), &n)) out.failed = static_cast<long long>(n);
            } else if (w == "cancelled") {
                out.cancelled = true;
            }
        }
        return out;
    }
    if (words.size() == 2) {
        unsigned long long d = 0, t = 0;
        if (parse_count(words[0], &d) && parse_count(words[1], &t)) {
            out.kind = PrepareKind::progress;
            out.done = d;
            out.total = t;
            out.text = rest;
            return out;
        }
    }
    out.text = line;
    return out;
}

}  // namespace nxfmt
