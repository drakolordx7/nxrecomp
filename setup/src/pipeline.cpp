// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <map>
#include <mutex>
#include <thread>

#include "container.h"
#include "crypto.h"
#include "fileio.h"
#include "nso.h"
#include "shadercache.h"
#include "texcache.h"

namespace mprs {

const char* stage_name(Stage s) {
    switch (s) {
        case Stage::Inspect: return "Checking the dump";
        case Stage::Space: return "Checking disk space";
        case Stage::Exefs: return "Copying the game program";
        case Stage::Romfs: return "Extracting game files";
        case Stage::Textures: return "Converting textures";
        case Stage::ShaderCache: return "Installing shaders";
        case Stage::Finish: return "Finishing";
        default: return "";
    }
}

namespace {

using Clock = std::chrono::steady_clock;
double secs_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

std::string gb(u64 b) {
    char s[32];
    std::snprintf(s, sizeof s, "%.1f GB", double(b) / 1e9);
    return s;
}

constexpr u64 kBc7Estimate = 4'500'000'000ull;       // measured 4.35 GB for the 4,369 ASTC textures of v1.0.0
constexpr u64 kShaderEstimate = 1'100'000'000ull;    // 1,008 MB raw SPIR-V + results.tsv
constexpr const char* kStateName = "setup_state.tsv";

struct FileRec {
    u64 size = 0;
    std::string sha256;   // hex, may be empty (file accepted by size only)
};

// setup_state.tsv: what an earlier run finished. Tab separated; unknown lines are kept out, damaged files are ignored.
class State {
public:
    explicit State(std::string path) : path_(std::move(path)) { load(); }
    std::map<std::string, std::string> meta;
    bool find(const std::string& rel, FileRec& out) {
        std::lock_guard lk(mu_);
        auto it = files_.find(rel);
        if (it == files_.end()) return false;
        out = it->second;
        return true;
    }
    void put(const std::string& rel, const FileRec& r) {
        std::lock_guard lk(mu_);
        files_[rel] = r;
        dirty_ = true;
    }
    void set_meta(const std::string& k, const std::string& v) {
        std::lock_guard lk(mu_);
        if (meta[k] != v) { meta[k] = v; dirty_ = true; }
    }
    std::string get_meta(const std::string& k) {
        std::lock_guard lk(mu_);
        auto it = meta.find(k);
        return it == meta.end() ? std::string() : it->second;
    }
    void save(bool force = false) {
        std::string text;
        {
            std::lock_guard lk(mu_);
            if (!dirty_ && !force) return;
            text = "mprs_setup_state\t1\n";
            for (auto& [k, v] : meta) text += "meta\t" + k + "\t" + v + "\n";
            for (auto& [k, r] : files_) text += "file\t" + k + "\t" + std::to_string(r.size) + "\t" + r.sha256 + "\n";
            dirty_ = false;
        }
        write_text_file_atomic(path_, text);
    }
private:
    void load() {
        std::string t;
        if (!read_text_file(path_, t)) return;
        size_t pos = 0;
        bool first = true;
        while (pos < t.size()) {
            size_t e = t.find('\n', pos);
            if (e == std::string::npos) e = t.size();
            std::string line = t.substr(pos, e - pos);
            pos = e + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> f;
            size_t p = 0;
            for (;;) {
                size_t q = line.find('\t', p);
                f.push_back(line.substr(p, q == std::string::npos ? q : q - p));
                if (q == std::string::npos) break;
                p = q + 1;
            }
            if (first) {
                first = false;
                if (f.size() < 2 || f[0] != "mprs_setup_state" || f[1] != "1") return;   // other version: start fresh
                continue;
            }
            if (f.size() >= 3 && f[0] == "meta") meta[f[1]] = f[2];
            else if (f.size() >= 4 && f[0] == "file") files_[f[1]] = FileRec{std::strtoull(f[2].c_str(), nullptr, 10), f[3]};
        }
    }
    std::string path_;
    std::mutex mu_;
    std::map<std::string, FileRec> files_;
    bool dirty_ = false;
};

struct Opened {
    KeySet keys;
    std::unique_ptr<Container> c;
    DumpTitle t;
    std::unique_ptr<Pfs> exefs;
    std::unique_ptr<Romfs> rom;
    std::string build_id;
    std::string version;
};

void load_keys(const SetupOptions& o, KeySet& keys) {
    std::vector<std::string> files = o.key_files;
    if (files.empty()) {
        auto found = find_key_files();
        if (found.empty())
            fail(Fail::Keys, "No Switch keys file was found. Pick your prod.keys file (dumped from your own Switch with "
                             "Lockpick_RCM), or put it next to this program.");
        files.push_back(found.front().path);
        MPRS_LOGI("Using the keys found in the %s folder.", found.front().source.c_str());
        std::string tk = sibling_title_keys(found.front().path);
        if (!tk.empty()) files.push_back(tk);
    }
    for (const std::string& f : files) keys.load_file(f);
    MPRS_LOGI("Keys loaded: %s", keys.describe().c_str());
}

std::unique_ptr<Opened> open_dump(const SetupOptions& o) {
    auto d = std::make_unique<Opened>();
    load_keys(o, d->keys);
    d->c = open_container(o.input_path);
    MPRS_LOGI("Dump: %s, %zu archives", d->c->kind.c_str(), d->c->ncas.size());
    d->t = find_program(*d->c, d->keys, o.expected_title_id);
    if (d->t.have_version && d->t.title_version != 0)
        fail(Fail::Input, "This is version %u of the game. Only the original v1.0.0 release is supported (updates change the "
                          "game program).", d->t.title_version);
    d->version = d->t.have_version ? "1.0.0" : "";
    d->exefs = d->t.program->open_pfs0(0);
    for (const char* need : {"main", "rtld", "sdk", "subsdk0", "main.npdm"})
        if (!d->exefs->find(need)) fail(Fail::Input, "The dump's game program is incomplete (\"%s\" is missing).", need);
    NsoInfo ni = nso_read_info(*d->exefs->open_file(*d->exefs->find("main")));
    d->build_id = ni.build_id();
    if (d->build_id != o.expected_build_id)
        fail(Fail::Input, "This is not the v1.0.0 version of the game (its program has build id %s, expected %s). Only the original v1.0.0 "
                          "release is supported.", d->build_id.c_str(), o.expected_build_id.c_str());
    int rom_section = -1;
    for (int i = 0; i < 4; ++i)
        if (const NcaSection* s = d->t.program->section(i))
            if (s->fs_type == 0 && s->hash_type == 3) { rom_section = i; break; }
    if (rom_section < 0) fail(Fail::Input, "The dump has no game data (RomFS) section.");
    d->rom = Romfs::open(d->t.program->open_romfs_image(rom_section));
    MPRS_LOGI("Game %s version %s, program %s, %zu files (%s)", o.expected_title_id.c_str(), d->version.c_str(), d->build_id.c_str(),
              d->rom->files().size(), gb(d->rom->total_file_bytes()).c_str());
    return d;
}

int rom_section_of(const Nca& n) {
    for (int i = 0; i < 4; ++i)
        if (const NcaSection* s = n.section(i))
            if (s->fs_type == 0 && s->hash_type == 3) return i;
    return -1;
}

u64 bytes_missing(const std::string& dir, const std::string& rel, u64 size) {
    std::string path = path_join(dir, rel);
    for (char& c : path) if (c == '/') c = '\\';
    return file_size_is(path, size) ? 0 : size;
}

// Copies src to dest through an atomic file, hashing on the way. Calls on_bytes(n) after every chunk.
std::string copy_to_file(const IStorage& src, const std::string& dest, const std::function<void(u64)>& on_bytes,
                         const std::function<bool()>& cancelled) {
    AtomicFileWriter w(dest);
    const u64 size = src.size();
    std::vector<u8> buf(4u << 20);
    for (u64 off = 0; off < size;) {
        if (cancelled()) fail(Fail::Cancelled, "Stopped by the user.");
        size_t n = static_cast<size_t>(std::min<u64>(buf.size(), size - off));
        src.read(off, buf.data(), n);
        w.write(buf.data(), n);
        off += n;
        on_bytes(n);
    }
    return hex_encode(w.finish().data(), 32);
}

std::string sha_of_storage(const IStorage& s, const std::function<bool()>& cancelled) {
    Sha256 h;
    std::vector<u8> buf(4u << 20);
    for (u64 off = 0; off < s.size();) {
        if (cancelled()) fail(Fail::Cancelled, "Stopped by the user.");
        size_t n = static_cast<size_t>(std::min<u64>(buf.size(), s.size() - off));
        s.read(off, buf.data(), n);
        h.update(buf.data(), n);
        off += n;
    }
    return hex_encode(h.finish().data(), 32);
}

std::string sha_of_file(const std::string& path, const std::function<bool()>& cancelled) {
    return sha_of_storage(*open_file_storage(path), cancelled);
}

std::string to_win(std::string p) {
    for (char& c : p) if (c == '/') c = '\\';
    return p;
}

// Is `dest` (relative name `rel`) already the file we would write? Size always; hash too when verifying.
bool file_is_done(const std::string& dest, const std::string& rel, u64 size, State& st, bool verify, const IStorage* src,
                  const std::function<bool()>& cancelled, std::string& sha_out) {
    if (!file_size_is(dest, size)) return false;
    FileRec r;
    bool known = st.find(rel, r) && r.size == size && !r.sha256.empty();
    if (known) sha_out = r.sha256;
    if (!verify) return true;
    std::string have = sha_of_file(dest, cancelled);
    std::string want = known ? r.sha256 : (src ? sha_of_storage(*src, cancelled) : std::string());
    if (want.empty() || have != want) return false;
    sha_out = have;
    return true;
}

std::string find_shader_source(const SetupOptions& o) {
    if (!o.shader_cache_src.empty()) return o.shader_cache_src;
    std::string d = path_join(exe_dir(), "shader_cache");
    if (path_exists(path_join(d, "results.tsv"))) return d;
    std::string p = path_join(exe_dir(), "shader_cache.mprshc");
    if (path_exists(p)) return p;
    return "";
}

std::string make_ini(int quality, bool with_shaders) {
    std::string s =
        "; Written by mpr_setup. Setup rewrites this file only while this first line is present; delete the line to keep your edits.\n"
        "[fs]\nromfs = romfs\n"
        "[nvn]\nbc7_cache = bc7_cache\nbc7_quality = " + std::to_string(quality) + "\n";
    if (with_shaders) s += "shader_cache = shader_cache/spirv\n";
    return s;
}

}  // namespace

InputSummary inspect_input(const SetupOptions& o) {
    InputSummary s;
    try {
        auto d = open_dump(o);
        s.container = d->c->kind;
        s.title_id = o.expected_title_id;
        s.version = d->version;
        s.build_id = d->build_id;
        s.romfs_files = static_cast<u32>(d->rom->files().size());
        s.romfs_bytes = d->rom->total_file_bytes();
        for (auto& e : d->exefs->entries()) s.exefs_bytes += e.size;
        s.bc7_bytes_estimate = kBc7Estimate;
        State st(path_join(o.out_dir, kStateName));
        u64 need = 0;
        for (auto& e : d->exefs->entries()) need += bytes_missing(path_join(o.out_dir, "exefs"), e.name, e.size);
        for (auto& f : d->rom->files()) need += bytes_missing(path_join(o.out_dir, "romfs"), f.path, f.size);
        if (!o.skip_textures && st.get_meta("textures_done").empty()) need += kBc7Estimate;
        if (!find_shader_source(o).empty()) need += kShaderEstimate;
        s.disk_needed = need + need / 50;
        s.disk_free = o.out_dir.empty() ? 0 : free_disk_bytes(o.out_dir);
        s.supported = true;
        if (s.disk_free && s.disk_free < s.disk_needed) {
            s.supported = false;
            s.problem = "Not enough free space: setup needs about " + gb(s.disk_needed) + " and the drive has " + gb(s.disk_free) +
                        " free.";
        }
    } catch (const Error& e) {
        if (e.kind == Fail::Cancelled) throw;
        s.supported = false;
        s.problem = e.what();
    }
    return s;
}

SetupReport run_setup(const SetupOptions& o, Reporter& r) {
    SetupReport rep;
    const auto t_all = Clock::now();
    if (o.out_dir.empty()) fail(Fail::Generic, "No install folder was chosen.");
    if (!create_dirs(o.out_dir)) fail(Fail::Disk, "Cannot create the install folder \"%s\".", o.out_dir.c_str());
    log_open(path_join(o.out_dir, "setup.log"));
    struct LogCloser { ~LogCloser() { log_close(); } } log_closer;
    MPRS_LOGI("---- setup started (quality %d, threads %d) ----", o.bc7_quality, o.threads);
    auto cancelled = [&] { return r.cancel_requested(); };
    auto check_cancel = [&] { if (cancelled()) fail(Fail::Cancelled, "Stopped by the user. Run setup again to continue where it left off."); };
    State st(path_join(o.out_dir, kStateName));
    for (const char* sub : {"exefs", "romfs", "bc7_cache", "shader_cache"})
        if (int n = remove_part_files(path_join(o.out_dir, sub))) MPRS_LOGI("Removed %d unfinished file(s) from an earlier run in %s.", n, sub);
    auto begin = [&](Stage s, u64 total, const char* unit) {
        MPRS_LOGI("%s", stage_name(s));
        r.stage_begin(s, stage_name(s), total, unit);
        return Clock::now();
    };
    auto end = [&](Stage s, Clock::time_point t0) {
        double sec = secs_since(t0);
        rep.seconds[static_cast<int>(s)] += sec;
        r.stage_end(s, sec);
    };

    try {
        // ---- Inspect ---------------------------------------------------------------------------------------------------
        auto t0 = begin(Stage::Inspect, 0, "bytes");
        auto d = open_dump(o);
        if (o.verify_input) {
            MPRS_LOGI("Checking the dump's own hashes (this reads the whole file once).");
            const int rs = rom_section_of(*d->t.program);
            u64 total = 0;
            for (int i : {0, rs}) if (const NcaSection* s = d->t.program->section(i)) total += s->size;
            r.stage_begin(Stage::Inspect, stage_name(Stage::Inspect), total, "bytes");
            u64 base = 0;
            for (int i : {0, rs}) {
                const NcaSection* s = d->t.program->section(i);
                if (!s) continue;
                u64 b0 = base;
                d->t.program->verify_section(i, [&](u64 done, u64 tot) { r.stage_progress(b0 + done * s->size / std::max<u64>(tot, 1)); }, cancelled);
                base += s->size;
            }
            MPRS_LOGI("The dump's hashes are all correct.");
        }
        end(Stage::Inspect, t0);
        if (o.inspect_only) { rep.total_seconds = secs_since(t_all); return rep; }
        check_cancel();

        st.set_meta("title_id", o.expected_title_id);
        st.set_meta("build_id", d->build_id);

        // ---- Space -----------------------------------------------------------------------------------------------------
        t0 = begin(Stage::Space, 0, "");
        {
            const std::string exedir = path_join(o.out_dir, "exefs"), romdir = path_join(o.out_dir, "romfs");
            u64 need = 0;
            for (auto& e : d->exefs->entries()) need += bytes_missing(exedir, e.name, e.size);
            for (auto& f : d->rom->files()) need += bytes_missing(romdir, f.path, f.size);
            if (!o.skip_textures && st.get_meta("textures_done").empty()) need += kBc7Estimate;
            if (!find_shader_source(o).empty()) need += kShaderEstimate;
            need += need / 50 + (64u << 20);
            u64 freeb = free_disk_bytes(o.out_dir);
            MPRS_LOGI("Disk space: need about %s, free %s.", gb(need).c_str(), gb(freeb).c_str());
            if (freeb < need)
                fail(Fail::Disk, "Not enough free space on the drive: setup needs about %s more and %s is free. Free some space or "
                                 "pick another folder.", gb(need).c_str(), gb(freeb).c_str());
        }
        end(Stage::Space, t0);
        check_cancel();

        // ---- ExeFS -----------------------------------------------------------------------------------------------------
        {
            u64 total = 0;
            for (auto& e : d->exefs->entries()) total += e.size;
            t0 = begin(Stage::Exefs, total, "bytes");
            std::atomic<u64> done{0};
            for (const PfsEntry& e : d->exefs->entries()) {
                const std::string rel = "exefs/" + e.name;
                const std::string dest = to_win(path_join(o.out_dir, rel));
                StoragePtr src = d->exefs->open_file(e);
                std::string sha;
                if (file_is_done(dest, rel, e.size, st, o.verify_output, src.get(), cancelled, sha)) {
                    ++rep.files_skipped;
                } else {
                    sha = copy_to_file(*src, dest, [&](u64 n) { r.stage_progress(done += n); rep.bytes_written += n; }, cancelled);
                    ++rep.files_written;
                }
                st.put(rel, FileRec{e.size, sha});
            }
            r.stage_progress(total);
            st.save();
            end(Stage::Exefs, t0);
        }
        check_cancel();

        // ---- RomFS -----------------------------------------------------------------------------------------------------
        {
            const u64 total = d->rom->total_file_bytes();
            t0 = begin(Stage::Romfs, total, "bytes");
            const std::string romdir = path_join(o.out_dir, "romfs");
            create_dirs(romdir);
            for (const std::string& dir : d->rom->dirs()) create_dirs(to_win(path_join(romdir, dir)));
            std::vector<const RomfsFile*> order;
            for (auto& f : d->rom->files()) order.push_back(&f);
            std::sort(order.begin(), order.end(), [](const RomfsFile* a, const RomfsFile* b) { return a->offset < b->offset; });
            std::atomic<size_t> next{0};
            std::atomic<u64> done{0}, written{0}, wfiles{0}, sfiles{0};
            std::atomic<bool> stop{false};
            std::atomic<int> active{0};
            std::exception_ptr first_error;
            std::mutex err_mu;
            int nthreads = o.threads > 0 ? std::min(o.threads, 8) : 3;
            active = nthreads;
            auto worker = [&] {
                try {
                    for (;;) {
                        if (stop) return;
                        size_t i = next++;
                        if (i >= order.size()) return;
                        const RomfsFile& f = *order[i];
                        const std::string rel = "romfs/" + f.path;
                        const std::string dest = to_win(path_join(o.out_dir, rel));
                        StoragePtr src = d->rom->open_file(f);
                        std::string sha;
                        if (file_is_done(dest, rel, f.size, st, o.verify_output, src.get(), [&] { return stop.load() || cancelled(); }, sha)) {
                            r.stage_progress(done += f.size);
                            ++sfiles;
                        } else {
                            sha = copy_to_file(*src, dest, [&](u64 n) { r.stage_progress(done += n); written += n; },
                                               [&] { return stop.load() || cancelled(); });
                            ++wfiles;
                        }
                        st.put(rel, FileRec{f.size, sha});
                    }
                } catch (...) {
                    std::lock_guard lk(err_mu);
                    if (!first_error) first_error = std::current_exception();
                    stop = true;
                }
            };
            std::vector<std::thread> pool;
            for (int i = 0; i < nthreads; ++i) pool.emplace_back([&] { worker(); --active; });
            while (active.load() > 0) {   // save the bookkeeping now and then so a killed run keeps most of it
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                st.save();
            }
            for (auto& t : pool) t.join();
            rep.bytes_written += written;
            rep.files_written += static_cast<u32>(wfiles.load());
            rep.files_skipped += static_cast<u32>(sfiles.load());
            st.save();
            if (first_error) std::rethrow_exception(first_error);
            end(Stage::Romfs, t0);
            MPRS_LOGI("RomFS: %llu files written, %llu already present.", (unsigned long long)wfiles.load(), (unsigned long long)sfiles.load());
        }
        check_cancel();

        // ---- Textures --------------------------------------------------------------------------------------------------
        if (!o.skip_textures) {
            t0 = begin(Stage::Textures, 0, "textures");
            const std::string cache_dir = path_join(o.out_dir, "bc7_cache");
            const std::string prev = st.get_meta("textures_done");   // "<quality>:<converted+skipped>"
            const std::string want_prefix = std::to_string(o.bc7_quality) + ":";
            if (!o.verify_output && !prev.empty() && prev.compare(0, want_prefix.size(), want_prefix) == 0) {
                MPRS_LOGI("Texture cache already built (%s textures at quality %d).", prev.substr(want_prefix.size()).c_str(), o.bc7_quality);
                ++rep.textures_skipped;
            } else {
                TexCacheOptions to;
                to.romfs_dir = path_join(o.out_dir, "romfs");
                to.cache_dir = cache_dir;
                to.quality = o.bc7_quality;
                to.threads = o.threads;
                bool announced = false;
                u64 last_total = 0;
                to.weighted_progress = [&](u64 done_w, u64 total_w) {
                    const u64 total_mp = total_w / 1000000 + 1;
                    if (!announced || total_mp != last_total) {
                        announced = true;
                        last_total = total_mp;
                        r.stage_begin(Stage::Textures, "Converting textures", total_mp, "megapixels");
                    }
                    r.stage_progress(done_w / 1000000);
                };
                TexCacheStats ts = build_texture_cache(to, [](u64, u64) {}, cancelled);
                rep.textures_total = ts.textures_total;
                rep.textures_converted = ts.converted;
                rep.textures_skipped = ts.skipped;
                rep.textures_failed = ts.failed;
                rep.bytes_written += ts.bytes_written;
                MPRS_LOGI("Textures: %u total, %u converted, %u already done, %u failed, %s written, %.0f s.", ts.textures_total,
                          ts.converted, ts.skipped, ts.failed, gb(ts.bytes_written).c_str(), ts.seconds);
                if (ts.failed)
                    fail(Fail::Generic, "%u textures could not be converted (see setup.log). Run setup again; if it keeps failing "
                                        "the dump may be damaged.", ts.failed);
                st.set_meta("textures_done", want_prefix + std::to_string(ts.converted + ts.skipped));
                st.save();
            }
            end(Stage::Textures, t0);
        }
        check_cancel();

        // ---- Shader cache ----------------------------------------------------------------------------------------------
        bool shaders = false;
        {
            const std::string src = find_shader_source(o);
            t0 = begin(Stage::ShaderCache, 0, "files");
            if (src.empty()) {
                MPRS_LOGI("No shader cache supplied with this build; skipping (the game compiles shaders as it needs them).");
            } else {
                bool announced = false;
                ShaderInstallResult sr = install_shader_cache(
                    src, path_join(o.out_dir, "shader_cache\\spirv"), o.verify_output,
                    [&](u64 done, u64 total) {
                        if (!announced) { announced = true; r.stage_begin(Stage::ShaderCache, stage_name(Stage::ShaderCache), total, "files"); }
                        r.stage_progress(done);
                    },
                    cancelled);
                shaders = true;
                rep.bytes_written += sr.bytes_written;
                MPRS_LOGI("Shaders: %u files, %u installed, %u already present.", sr.files_total, sr.files_installed, sr.files_skipped);
            }
            end(Stage::ShaderCache, t0);
        }

        // ---- Finish ----------------------------------------------------------------------------------------------------
        t0 = begin(Stage::Finish, 0, "");
        {
            const std::string ini_path = path_join(o.out_dir, "mpr.ini");
            std::string existing;
            bool ours = !read_text_file(ini_path, existing) || existing.rfind("; Written by mpr_setup", 0) == 0;
            if (ours) write_text_file_atomic(ini_path, make_ini(o.bc7_quality, shaders));
            else MPRS_LOGW("mpr.ini was edited by hand; left alone. It must contain bc7_quality = %d.", o.bc7_quality);
            st.set_meta("bc7_quality", std::to_string(o.bc7_quality));
            st.set_meta("finished", "1");
            st.save(true);
        }
        end(Stage::Finish, t0);
        rep.total_seconds = secs_since(t_all);
        MPRS_LOGI("---- setup finished in %.0f s ----", rep.total_seconds);
        return rep;
    } catch (const Error& e) {
        st.save();
        MPRS_LOGE("Setup stopped: %s", e.what());
        throw;
    } catch (const std::exception& e) {
        st.save();
        MPRS_LOGE("Setup failed: %s", e.what());
        throw Error(Fail::Generic, e.what());
    }
}

}  // namespace mprs
