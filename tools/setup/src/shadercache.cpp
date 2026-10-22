// SPDX-License-Identifier: GPL-3.0-or-later
#include "shadercache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "crypto.h"
#include "fileio.h"
#include "storage.h"

#ifdef MPRS_HAVE_ZSTD
#include <zstd.h>
#endif

namespace mprs {
namespace {

constexpr char kPackMagic[8] = {'M', 'P', 'R', 'S', 'H', 'C', '0', '1'};
constexpr u32 kMaxPackEntries = 1u << 20;
constexpr u64 kPackRecordFixed = 8 + 8 + 8 + 32 + 1;   // data_offset, stored_size, raw_size, sha256, method
constexpr const char* kResults = "results.tsv";
constexpr const char* kManifest = "manifest.sha256";
// A method-1 entry declares its raw size in the record, so that size is not bounded by the file. Shader modules are a few KB;
// this cap keeps a lying record from asking for an absurd allocation.
constexpr u64 kMaxRawEntryBytes = u64(1) << 30;
#ifndef MPRS_HAVE_ZSTD
constexpr const char* kZstdMessage = "This shader pack is compressed with zstd, which this build cannot read";
#endif

// Decodes one zstd-compressed entry (in_len bytes) into out, which must be exactly out_len bytes. Returns false if the data is not
// a zstd stream, or does not decode to exactly out_len bytes (a damaged payload or a raw_size that does not match it).
bool decompress_zstd(const u8* in, size_t in_len, u8* out, size_t out_len) {
#ifdef MPRS_HAVE_ZSTD
    u8 spare = 0;   // ZSTD_decompress wants a valid pointer even when the entry is empty
    const size_t got = ZSTD_decompress(out_len != 0 ? out : &spare, out_len, in, in_len);
    return !ZSTD_isError(got) && got == out_len;
#else
    (void)in;
    (void)in_len;
    (void)out;
    (void)out_len;
    return false;   // read_pack() refuses method-1 entries in this build, so this is not reached
#endif
}

u16 le16(const u8* p) { return static_cast<u16>(p[0] | p[1] << 8); }
u32 le32(const u8* p) {
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 | static_cast<u32>(p[3]) << 24;
}
u64 le64(const u8* p) { return le32(p) | static_cast<u64>(le32(p + 4)) << 32; }

// "results.tsv", or 16 lower-case hex digits + ".spv".
bool valid_cache_name(std::string_view n) {
    if (n == kResults) return true;
    if (n.size() != 20 || n.substr(16) != ".spv") return false;
    for (size_t i = 0; i < 16; ++i) {
        char c = n[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// One file to install. For a folder source src_path is set; for a pack, off/stored/method say where the bytes are.
struct Item {
    std::string name;
    u64 size = 0;             // raw (installed) size
    bool has_hash = false;
    std::array<u8, 32> hash{};
    std::string src_path;
    u64 off = 0, stored = 0;
    u8 method = 0;
};

std::array<u8, 32> digest_of(const std::vector<u8>& v) {
    static const u8 kEmpty[1] = {0};
    return sha256(v.empty() ? kEmpty : v.data(), v.size());
}

// Reads manifest.sha256 ("<64 hex>  <file name>" per line) and attaches a hash to every item. Every file must be listed and every
// listed file must exist.
void apply_manifest(const std::string& src, std::vector<Item>& items) {
    std::string text;
    std::string path = path_join(src, kManifest);
    if (!read_text_file(path, text)) fail(Fail::Input, "Cannot read \"%s\".", path.c_str());
    std::map<std::string, std::array<u8, 32>> listed;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) continue;
        std::array<u8, 32> h{};
        if (line.size() < 66 || !hex_decode(std::string_view(line).substr(0, 64), h.data(), h.size()))
            fail(Fail::Input, "manifest.sha256 has a line that is not a checksum: \"%s\".", line.substr(0, 80).c_str());
        size_t p = 64;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '*')) ++p;
        std::string name = line.substr(p);
        if (!valid_cache_name(name))
            fail(Fail::Input, "manifest.sha256 names \"%s\", which is not part of a shader cache.", name.c_str());
        if (!listed.emplace(name, h).second) fail(Fail::Input, "manifest.sha256 lists \"%s\" twice.", name.c_str());
    }
    for (Item& it : items) {
        auto f = listed.find(it.name);
        if (f == listed.end()) fail(Fail::Input, "\"%s\" is not listed in manifest.sha256.", it.name.c_str());
        it.has_hash = true;
        it.hash = f->second;
        listed.erase(f);
    }
    if (!listed.empty())
        fail(Fail::Input, "manifest.sha256 lists \"%s\", but that file is not in the shader cache.", listed.begin()->first.c_str());
}

// Folder source: every file must be results.tsv, a cache .spv, or the manifest.
std::vector<Item> scan_folder(const std::string& src) {
    struct Raw {
        std::string name;
        bool dir;
        u64 size;
    };
    std::vector<Raw> raw;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(widen(path_join(src, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) fail(Fail::Input, "Cannot read the folder \"%s\".", src.c_str());
    do {
        std::string name = narrow(fd.cFileName);
        if (name == "." || name == "..") continue;
        u64 size = (static_cast<u64>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        raw.push_back({name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, size});
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    bool has_results = std::any_of(raw.begin(), raw.end(), [](const Raw& r) { return r.name == kResults; });
    if (!has_results) fail(Fail::Input, "%s does not contain a shader cache (results.tsv is missing)", src.c_str());

    std::vector<Item> items;
    bool has_manifest = false;
    for (const Raw& r : raw) {
        if (r.dir) fail(Fail::Input, "\"%s\" is a folder inside the shader cache, which is not expected.", r.name.c_str());
        if (r.name == kManifest) {
            has_manifest = true;
            continue;
        }
        if (!valid_cache_name(r.name)) fail(Fail::Input, "\"%s\" is not part of a shader cache.", r.name.c_str());
        Item it;
        it.name = r.name;
        it.size = r.size;
        it.src_path = path_join(src, r.name);
        items.push_back(std::move(it));
    }
    if (has_manifest) apply_manifest(src, items);
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.name < b.name; });
    return items;
}

// Pack source: parses the whole record table first, so a bad pack is refused before anything is written.
std::vector<Item> read_pack(const IStorage& st, const std::string& path) {
    const u64 total = st.size();
    auto bad = [&](const char* why) { fail(Fail::Input, "\"%s\" is not a usable shader pack (%s).", path.c_str(), why); };
    if (total < 16) bad("it is too short");
    u8 hdr[16];
    st.read(0, hdr, sizeof hdr);
    if (std::memcmp(hdr, kPackMagic, sizeof kPackMagic) != 0) bad("the signature is wrong");
    u32 count = le32(hdr + 8);
    if (count > kMaxPackEntries) bad("the entry count is too large");

    std::vector<Item> items;
    std::set<std::string> seen;
    u64 pos = 16;
    for (u32 i = 0; i < count; ++i) {
        if (total - pos < 2) bad("the record table is cut short");
        u8 l[2];
        st.read(pos, l, sizeof l);
        pos += 2;
        u16 name_len = le16(l);
        if (name_len == 0 || total - pos < name_len + kPackRecordFixed) bad("the record table is cut short");
        std::string name(name_len, '\0');
        st.read(pos, name.data(), name_len);
        pos += name_len;
        u8 f[kPackRecordFixed];
        st.read(pos, f, sizeof f);
        pos += sizeof f;

        if (!valid_cache_name(name))
            fail(Fail::Input, "\"%s\" holds \"%s\", which is not part of a shader cache.", path.c_str(), name.c_str());
        if (!seen.insert(name).second) fail(Fail::Input, "\"%s\" holds \"%s\" twice.", path.c_str(), name.c_str());
        Item it;
        it.name = name;
        it.off = le64(f);
        it.stored = le64(f + 8);
        it.size = le64(f + 16);
        std::memcpy(it.hash.data(), f + 24, it.hash.size());
        it.method = f[56];
        it.has_hash = true;
        if (it.method > 1) bad("an entry uses an unknown compression method");
#ifndef MPRS_HAVE_ZSTD
        if (it.method == 1) fail(Fail::Input, "%s", kZstdMessage);
#endif
        if (it.method == 0 && it.stored != it.size) bad("a stored entry has two different sizes");
        if (it.method == 1 && it.size > kMaxRawEntryBytes) bad("an entry declares a raw size larger than the installer accepts");
        if (it.off > total || it.stored > total - it.off) bad("an entry points past the end of the file");
        items.push_back(std::move(it));
    }
    if (!seen.count(kResults))
        fail(Fail::Input, "%s does not contain a shader cache (results.tsv is missing)", path.c_str());
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.name < b.name; });
    return items;
}

std::vector<u8> load_folder_entry(const Item& it) {
    StoragePtr s = open_file_storage(it.src_path);
    if (s->size() != it.size) fail(Fail::Input, "\"%s\" changed while it was being installed.", it.src_path.c_str());
    return read_all(*s, 0, static_cast<size_t>(it.size));
}

// Reads one pack entry and returns its raw bytes. Only this entry is held in memory.
std::vector<u8> load_pack_entry(const IStorage& st, const Item& it) {
    std::vector<u8> stored = read_all(st, it.off, static_cast<size_t>(it.stored));
    if (it.method == 0) return stored;
    std::vector<u8> raw(static_cast<size_t>(it.size));
    if (!decompress_zstd(stored.data(), stored.size(), raw.data(), raw.size()))
        fail(Fail::Input, "\"%s\" in the shader pack could not be decompressed (the pack is damaged, or its raw size is wrong).",
             it.name.c_str());
    return raw;
}

// True if the existing file at path has the given SHA-256. Any read problem counts as "no" (the file is then rewritten).
bool dest_matches(const std::string& path, const std::array<u8, 32>& digest) {
    try {
        StoragePtr s = open_file_storage(path);
        std::vector<u8> v = read_all(*s, 0, static_cast<size_t>(s->size()));
        return digest_of(v) == digest;
    } catch (const Error&) {
        return false;
    }
}

}  // namespace

ShaderInstallResult install_shader_cache(const std::string& src_utf8, const std::string& dest_dir_utf8, bool verify,
                                         const std::function<void(u64 done, u64 total)>& progress,
                                         const std::function<bool()>& cancelled) {
    DWORD attr = GetFileAttributesW(widen(src_utf8).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) fail(Fail::Input, "Cannot find the shader cache at \"%s\".", src_utf8.c_str());
    const bool folder = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

    std::vector<Item> items;
    StoragePtr pack;
    if (folder) {
        items = scan_folder(src_utf8);
    } else {
        pack = open_file_storage(src_utf8);
        items = read_pack(*pack, src_utf8);
    }

    ShaderInstallResult res;
    res.files_total = static_cast<u32>(items.size());
    res.had_manifest = folder ? (!items.empty() && items.front().has_hash) : true;   // a pack carries a checksum per file

    if (!create_dirs(dest_dir_utf8)) fail(Fail::Disk, "Cannot create the folder \"%s\".", dest_dir_utf8.c_str());

    const u64 total = items.size();
    u64 done = 0;
    for (const Item& it : items) {
        if (cancelled && cancelled()) fail(Fail::Cancelled, "The shader cache install was cancelled.");
        const std::string out = path_join(dest_dir_utf8, it.name);
        const bool present = file_size_is(out, it.size);
        if (present && !verify) {
            ++res.files_skipped;
        } else {
            // The source is read and checked before the destination is touched.
            std::vector<u8> data = folder ? load_folder_entry(it) : load_pack_entry(*pack, it);
            const std::array<u8, 32> digest = digest_of(data);
            if (it.has_hash && digest != it.hash)
                fail(Fail::Input, "\"%s\" does not match its checksum in the shader cache.", it.name.c_str());
            if (present && dest_matches(out, digest)) {
                ++res.files_skipped;
            } else {
                AtomicFileWriter w(out);
                w.write(data.data(), data.size());
                w.finish();
                ++res.files_installed;
                res.bytes_written += data.size();
            }
        }
        ++done;
        if (progress) progress(done, total);
    }
    return res;
}

}  // namespace mprs
