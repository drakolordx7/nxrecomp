// SPDX-License-Identifier: GPL-3.0-or-later
// Shader program list: see programs.h. Port of the program part of tools/survey/extract_shaders.py (split_raw, embedded_sets, add).
#include "programs.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include "crypto.h"
#include "fileio.h"
#include "log.h"
// Quoted path relative to this file: bc7enc_rdo (textures) ships an older miniz.h that would win on the include path.
#include "../third_party/miniz/miniz.h"
#include "pak.h"
#include "storage.h"

namespace fs = std::filesystem;

namespace mprs {
namespace {

constexpr u32 kComputeWord = 0x12345679;     // first word of a compute code block (the other code magic is 0x12345678)
constexpr size_t kCtrlCodeSizeAt = 0x704;    // control section: size of its code block
constexpr size_t kCtrlStartAt = 0x708;       // control section: offset of the program header (SPH) in the code block
constexpr size_t kCtrlMinSize = 0x70c;       // the two words above must lie inside the control section
constexpr size_t kEmbedMatchLen = 15;        // 00 | 4 bytes | 11|19|08 00 00 00 | 4 bytes | 78 flag
constexpr size_t kInflateHintMax = 64u << 20;

u32 rd32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u64 rd64(const u8* p) { return u64(rd32(p)) | u64(rd32(p + 4)) << 32; }
void put32(u8* p, u32 v) {
    for (int i = 0; i < 4; ++i) p[i] = u8(v >> (8 * i));
}
void put64(u8* p, u64 v) {
    for (int i = 0; i < 8; ++i) p[i] = u8(v >> (8 * i));
}

bool is_code_magic(const u8* p) {
    static const u8 a[4] = {0x78, 0x56, 0x34, 0x12}, b[4] = {0x79, 0x56, 0x34, 0x12};
    return std::memcmp(p, a, 4) == 0 || std::memcmp(p, b, 4) == 0;
}
bool is_ctrl_magic(const u8* p) {
    static const u8 m[4] = {0x34, 0x12, 0x76, 0x98};
    return std::memcmp(p, m, 4) == 0;
}

struct TinflFree {
    void operator()(tinfl_decompressor* d) const { tinfl_decompressor_free(d); }
};

// Inflates one zlib stream (header and adler32 included) from src[0..n). used = input bytes the stream occupied (so used == n means
// the stream ends exactly at the end of the given bytes). False if it does not decode completely.
bool zlib_inflate(const u8* src, size_t n, std::vector<u8>& out, size_t& used, size_t size_hint) {
    std::unique_ptr<tinfl_decompressor, TinflFree> dec(tinfl_decompressor_alloc());
    if (!dec) return false;
    const mz_uint flags = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
    out.assign(std::max<size_t>(std::min(size_hint, kInflateHintMax), size_t(1) << 16), 0);
    size_t in_pos = 0, out_pos = 0;
    for (;;) {
        size_t in_size = n - in_pos;
        size_t out_size = out.size() - out_pos;
        const tinfl_status st =
            tinfl_decompress(dec.get(), src + in_pos, &in_size, out.data(), out.data() + out_pos, &out_size, flags);
        in_pos += in_size;
        out_pos += out_size;
        if (st == TINFL_STATUS_DONE) {
            out.resize(out_pos);
            used = in_pos;
            return true;
        }
        if (st != TINFL_STATUS_HAS_MORE_OUTPUT) return false;   // corrupt, or the input ends before the stream does
        out.resize(out.size() * 2);
    }
}

struct Block {
    const u8* code;
    size_t code_len;
    const u8* ctrl;
    size_t ctrl_len;
};

// split_raw of the Python extractor: code blocks are the 4-aligned code magics, control sections the 4-aligned 34 12 76 98 words.
// The counts must match, the control section must name the size of its own code block. False if the set does not split.
bool split_blocks(const std::vector<u8>& raw, std::vector<Block>& out) {
    const size_t n = raw.size();
    std::vector<size_t> codes, ctrls;
    for (size_t j = 0; j + 4 <= n; j += 4) {
        if (is_code_magic(&raw[j])) codes.push_back(j);
        else if (is_ctrl_magic(&raw[j])) ctrls.push_back(j);
    }
    if (codes.empty() || codes.size() != ctrls.size()) return false;
    const size_t count = codes.size();
    out.clear();
    for (size_t k = 0; k < count; ++k) {
        const size_t c_end = k + 1 < count ? codes[k + 1] : ctrls[0];
        const size_t t_end = k + 1 < count ? ctrls[k + 1] : n;
        if (c_end <= codes[k] || t_end <= ctrls[k]) return false;
        Block b{&raw[codes[k]], c_end - codes[k], &raw[ctrls[k]], t_end - ctrls[k]};
        if (b.ctrl_len < kCtrlMinSize) return false;
        if (rd32(b.ctrl + kCtrlCodeSizeAt) != b.code_len) return false;
        out.push_back(b);
    }
    return true;
}

// The hash of a stage: first 8 bytes of SHA-1 of the code block bytes, big-endian.
u64 block_hash(const Block& b) {
    const std::array<u8, 20> d = sha1(b.code, b.code_len);
    u64 h = 0;
    for (int i = 0; i < 8; ++i) h = (h << 8) | d[i];
    return h;
}

// NVN stage of a non-compute block: the stage field of the word at code[start]. Returns -1 when the word is out of range.
int block_stage(const Block& b) {
    const u32 start = rd32(b.ctrl + kCtrlStartAt);
    if (u64(start) + 4 > b.code_len) return -1;
    const u32 word = rd32(b.code + start);
    return int((word >> 10) & 15);
}

// The zlib body of an MTRL asset (mtrl_blob of the Python extractor), then inflated. The body is named by five u32 (cnt, ver,
// compressedSize, decompressedSize, fileOffset; fileOffset counts from the start of the asset bytes). In a pak those five words are
// the asset's own META record in the table of contents (meta, from Pak::read_meta). The extracted .MTRL files of the survey carry the
// same five words in a META footer form appended after the asset; that footer is the fallback when the record is absent.
// Throws Error(Input) with the reason when the body is missing or does not inflate.
void mtrl_blob(const std::vector<u8>& a, const std::vector<u8>& meta, std::vector<u8>& blob) {
    const u8* w = nullptr;   // the five words
    if (meta.size() >= 20) {
        w = meta.data();
    } else {
        if (a.size() < 32) fail(Fail::Input, "asset shorter than its form header");
        const u64 size = rd64(a.data() + 4);
        if (size > a.size()) fail(Fail::Input, "asset form size runs past the asset");
        const u64 off = 32 + size;
        if (off + 32 > a.size()) fail(Fail::Input, "no META record and no footer after the asset form");
        const u64 fsz = rd64(a.data() + off + 4);
        if (fsz > a.size()) fail(Fail::Input, "footer size runs past the asset");
        const u64 end = off + 32 + fsz;
        if (end > a.size()) fail(Fail::Input, "footer runs past the asset");
        u64 o = off + 32;
        while (o + 24 <= end) {
            const u8* h = a.data() + o;
            const u64 csz = rd64(h + 4), sk = rd64(h + 16);
            if (csz > a.size() || sk > a.size()) fail(Fail::Input, "footer chunk runs past the asset");
            const u64 body = o + 24 + sk;
            if (body + 20 > a.size()) fail(Fail::Input, "footer chunk body runs past the asset");
            if (std::memcmp(h, "META", 4) == 0) {
                w = a.data() + body;
                break;
            }
            o += 24 + sk + csz;
        }
        if (!w) fail(Fail::Input, "no META record and no META footer chunk");
    }
    const u32 cs = rd32(w + 8), ds = rd32(w + 12), fo = rd32(w + 16);
    if (fo > a.size() || cs > a.size() - fo)
        fail(Fail::Input, "zlib body (offset %u, size %u) is outside the asset of %zu bytes", fo, cs, a.size());
    size_t used = 0;
    if (!zlib_inflate(a.data() + fo, cs, blob, used, ds)) fail(Fail::Input, "zlib body (offset %u, size %u) does not inflate", fo, cs);
}

// Reads the SNVN chunk of MaterialArchive.arc and calls on_record(data, size) for each source record (read_sources of the Python).
// Returns false if the file is not an MTRL archive or has no SNVN chunk.
template <class F>
bool for_each_snvn_record(const std::vector<u8>& b, F&& on_record) {
    if (b.size() < 32 || std::memcmp(b.data(), "RFRM", 4) != 0 || std::memcmp(b.data() + 20, "MTRL", 4) != 0) return false;
    size_t body = 0;
    bool found = false;
    u64 off = 32;
    while (off + 24 <= b.size()) {
        const u64 size = rd64(&b[off + 4]), skip = rd64(&b[off + 16]);
        const u64 body_off = off + 24 + skip;
        if (std::memcmp(&b[off], "SNVN", 4) == 0) {
            body = static_cast<size_t>(body_off);
            found = true;
            break;
        }
        if (skip > b.size() || size > b.size() || body_off + size > b.size()) break;
        off = body_off + size;
    }
    if (!found || body + 8 > b.size()) return false;
    const u64 toc = rd32(&b[body + 4]);
    u64 p = body + 8 + toc;
    if (p + 4 > b.size()) return false;
    const u32 n = rd32(&b[p]);
    p += 4;
    for (u32 i = 0; i < n; ++i) {
        if (p + 5 > b.size()) return false;
        const u32 sl = rd32(&b[p + 1]);
        p += 5;
        if (p + sl + 4 > b.size()) return false;
        p += sl;
        const u32 dl = rd32(&b[p]);
        p += 4;
        if (p + dl > b.size()) return false;
        on_record(b.data() + p, size_t(dl));
        p += dl;
    }
    return true;
}

bool is_pak_path(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    for (wchar_t& c : e) c = static_cast<wchar_t>(std::towlower(c));
    return e == L".pak";
}

}  // namespace

SetResult program_of_raw_set(const std::vector<u8>& raw, ProgramHashes& out, u32* unknown_stage_blocks) {
    std::vector<Block> blocks;
    if (!split_blocks(raw, blocks)) return SetResult::Invalid;
    // Block indices by stage: 0 vertex, 1 tessellation control, 2 tessellation evaluation, 3 geometry, 4 fragment.
    std::vector<size_t> by_stage[5];
    bool compute = false;
    for (size_t k = 0; k < blocks.size(); ++k) {
        const Block& b = blocks[k];
        if (rd32(b.code) == kComputeWord) {
            compute = true;
            continue;
        }
        const int st = block_stage(b);
        if (st < 0) return SetResult::Invalid;
        switch (st) {
            case 0:
            case 1: by_stage[0].push_back(k); break;
            case 2: by_stage[1].push_back(k); break;
            case 3: by_stage[2].push_back(k); break;
            case 4: by_stage[3].push_back(k); break;
            case 5: by_stage[4].push_back(k); break;
            default:
                if (unknown_stage_blocks) ++*unknown_stage_blocks;
                break;
        }
    }
    if (compute || !by_stage[1].empty() || !by_stage[2].empty()) return SetResult::Skipped;
    for (const auto& v : by_stage)
        if (v.size() > 1) return SetResult::Skipped;
    if (by_stage[0].empty() || by_stage[4].empty()) return SetResult::Skipped;
    out.vs = block_hash(blocks[by_stage[0][0]]);
    out.fs = block_hash(blocks[by_stage[4][0]]);
    out.gs = by_stage[3].empty() ? 0 : block_hash(blocks[by_stage[3][0]]);
    return SetResult::Program;
}

std::vector<std::vector<u8>> embedded_raw_sets(const std::vector<u8>& blob) {
    std::vector<std::vector<u8>> out;
    const u8* d = blob.data();
    const size_t n = blob.size();
    auto len_tag = [](u8 v) { return v == 0x11 || v == 0x19 || v == 0x08; };
    auto zlib_flag = [](u8 v) { return v == 0x01 || v == 0x5e || v == 0x9c || v == 0xda; };
    size_t i = 0;
    // Same as finditer over EMBED_RE: a match is consumed whole (even when it is later rejected), otherwise the scan moves on one byte.
    while (i + kEmbedMatchLen <= n) {
        const bool match = d[i] == 0 && len_tag(d[i + 5]) && d[i + 6] == 0 && d[i + 7] == 0 && d[i + 8] == 0 && d[i + 13] == 0x78 &&
                           zlib_flag(d[i + 14]);
        if (!match) {
            ++i;
            continue;
        }
        const u32 zl = rd32(d + i + 9);
        const size_t st = i + 13;
        i += kEmbedMatchLen;
        if (zl > n - st) continue;
        std::vector<u8> raw;
        size_t used = 0;
        // The stream must end exactly at its declared length: bytes left over, or a stream that runs past it, are rejected.
        if (!zlib_inflate(d + st, zl, raw, used, size_t(zl) * 4) || used != zl) continue;
        if (raw.size() < 4 || !is_code_magic(raw.data())) continue;
        out.push_back(std::move(raw));
    }
    return out;
}

std::vector<ProgramHashes> scan_shader_programs(const std::string& romfs_dir_utf8, int threads,
                                                const std::function<void(u64 done, u64 total)>& progress,
                                                const std::function<bool()>& cancel, ProgramScanStats* stats) {
    ProgramScanStats local;
    ProgramScanStats& st = stats ? *stats : local;
    st = ProgramScanStats{};
    std::vector<ProgramHashes> all;
    auto cancelled = [&] { return cancel && cancel(); };
    auto stop_if_cancelled = [&] {
        if (cancelled()) fail(Fail::Cancelled, "Stopped by the user.");
    };

    const fs::path root(widen(romfs_dir_utf8));
    std::error_code ec;
    if (!fs::is_directory(root, ec)) fail(Fail::Input, "The game files folder %s does not exist.", romfs_dir_utf8.c_str());

    // 1. MaterialArchive.arc: the NVN program sets stored in its SNVN chunk.
    const std::string arc_path = narrow((root / L"MaterialArchive.arc").wstring());
    if (path_exists(arc_path)) {
        StoragePtr arc = open_file_storage(arc_path);
        const std::vector<u8> bytes = read_all(*arc, 0, static_cast<size_t>(arc->size()));
        const bool ok = for_each_snvn_record(bytes, [&](const u8* d, size_t dl) {
            ++st.arc.units;
            if (dl < 13) {
                ++st.arc_inflate_failed;
                return;
            }
            const u32 zl = rd32(d + 9);
            std::vector<u8> raw;
            size_t used = 0;
            if (zl > dl - 13 || !zlib_inflate(d + 13, zl, raw, used, size_t(zl) * 4)) {
                ++st.arc_inflate_failed;
                return;
            }
            ProgramHashes p;
            switch (program_of_raw_set(raw, p, &st.unknown_stage_blocks)) {
                case SetResult::Program: ++st.arc.programs; all.push_back(p); break;
                case SetResult::Skipped: ++st.arc.skipped; break;
                case SetResult::Invalid: ++st.arc.invalid; break;
            }
        });
        if (!ok) MPRS_LOGW("MaterialArchive.arc: the SNVN chunk is missing or damaged; some shader programs may be left out.");
        stop_if_cancelled();
    } else {
        MPRS_LOGW("MaterialArchive.arc not found in the game files: no shader programs taken from it.");
    }

    // 2. Every MTRL asset of every .pak under the RomFS folder.
    std::vector<fs::path> pak_paths;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && is_pak_path(it->path())) pak_paths.push_back(it->path());
    }
    std::sort(pak_paths.begin(), pak_paths.end());
    struct Job {
        std::shared_ptr<const Pak> pak;
        const PakEntry* entry;
    };
    std::vector<Job> jobs;
    for (const fs::path& pp : pak_paths) {
        const std::string utf8 = narrow(pp.wstring());
        std::shared_ptr<const Pak> pak;
        try {
            pak = Pak::open(open_file_storage(utf8));
        } catch (const std::exception& e) {
            ++st.pak_files_skipped;
            MPRS_LOGW("Skipping %s: %s", utf8.c_str(), e.what());
            continue;
        }
        for (const PakEntry& e : pak->assets())
            if (std::strcmp(e.type, "MTRL") == 0) jobs.push_back({pak, &e});
    }
    st.mtrl_assets = static_cast<u32>(jobs.size());
    stop_if_cancelled();

    const u64 total = jobs.size();
    std::atomic<size_t> next{0};
    std::atomic<u64> done{0};
    std::atomic<bool> stop{false};
    std::mutex merge_mu;
    std::string first_error;
    int nthreads = threads > 0 ? threads : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    nthreads = std::max(1, std::min(nthreads, static_cast<int>(std::max<size_t>(jobs.size(), 1))));

    auto worker = [&] {
        std::vector<ProgramHashes> found;
        ProgramSourceStats pst;
        u32 failed = 0, unknown = 0;
        std::string first_error_here;
        while (!stop.load()) {
            if (cancelled()) {
                stop = true;
                break;
            }
            const size_t i = next++;
            if (i >= jobs.size()) break;
            try {
                const std::vector<u8> form = jobs[i].pak->read_asset(*jobs[i].entry);
                const std::vector<u8> meta = jobs[i].pak->read_meta(*jobs[i].entry);
                std::vector<u8> blob;
                mtrl_blob(form, meta, blob);
                for (const std::vector<u8>& raw : embedded_raw_sets(blob)) {
                    ++pst.units;
                    ProgramHashes p;
                    switch (program_of_raw_set(raw, p, &unknown)) {
                        case SetResult::Program: ++pst.programs; found.push_back(p); break;
                        case SetResult::Skipped: ++pst.skipped; break;
                        case SetResult::Invalid: ++pst.invalid; break;
                    }
                }
            } catch (const std::exception& e) {
                ++failed;
                if (first_error_here.empty())
                    first_error_here = std::string(e.what()) + " (asset " + jobs[i].entry->id.str() + ")";
            }
            if (progress) progress(done.fetch_add(1) + 1, total);
        }
        std::lock_guard lk(merge_mu);
        if (first_error.empty()) first_error = first_error_here;
        all.insert(all.end(), found.begin(), found.end());
        st.pak.units += pst.units;
        st.pak.invalid += pst.invalid;
        st.pak.skipped += pst.skipped;
        st.pak.programs += pst.programs;
        st.mtrl_failed += failed;
        st.unknown_stage_blocks += unknown;
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(nthreads));
    for (int t = 0; t < nthreads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    stop_if_cancelled();
    st.first_asset_error = first_error;

    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    st.unique_programs = static_cast<u32>(all.size());
    return all;
}

void write_programs_file(const std::string& path_utf8, const std::vector<ProgramHashes>& programs) {
    std::vector<u8> buf(16 + 24 * programs.size());
    std::memcpy(buf.data(), "MPRPRG01", 8);
    put32(buf.data() + 8, static_cast<u32>(programs.size()));
    put32(buf.data() + 12, 0);
    for (size_t i = 0; i < programs.size(); ++i) {
        u8* p = buf.data() + 16 + 24 * i;
        put64(p, programs[i].vs);
        put64(p + 8, programs[i].fs);
        put64(p + 16, programs[i].gs);
    }
    AtomicFileWriter w(path_utf8);
    w.write(buf.data(), buf.size());
    w.finish();
}

std::vector<ProgramHashes> read_programs_file(const std::string& path_utf8) {
    StoragePtr f = open_file_storage(path_utf8);
    const u64 size = f->size();
    if (size < 16) fail(Fail::Input, "The shader program list is too short to be a program list (%llu bytes).", (unsigned long long)size);
    u8 h[16];
    f->read(0, h, sizeof h);
    if (std::memcmp(h, "MPRPRG01", 8) != 0) fail(Fail::Input, "The shader program list has the wrong header.");
    const u32 count = rd32(h + 8);
    if (rd32(h + 12) != 0) fail(Fail::Input, "The shader program list has a damaged header.");
    if (size != 16 + 24 * u64(count))
        fail(Fail::Input, "The shader program list holds %u programs but its size is %llu bytes.", count, (unsigned long long)size);
    const std::vector<u8> buf = read_all(*f, 16, static_cast<size_t>(size - 16));
    std::vector<ProgramHashes> out(count);
    for (u32 i = 0; i < count; ++i) {
        const u8* p = buf.data() + 24 * size_t(i);
        out[i] = ProgramHashes{rd64(p), rd64(p + 8), rd64(p + 16)};
        if (i > 0 && !(out[i - 1] < out[i])) fail(Fail::Input, "The shader program list is not sorted (record %u).", i);
    }
    return out;
}

}  // namespace mprs
