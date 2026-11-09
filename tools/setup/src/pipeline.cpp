// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <set>
#include <mutex>
#include <thread>

#include "container.h"
#include "crypto.h"
#include "fileio.h"
#include "game_setup.h"
#include "nso.h"
#include "nx_formats.h"
#include "shadercache.h"
#include "texcache.h"

namespace nxs {

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

DiskEstimate disk_estimate_of(const SetupOptions& o, const std::string& shader_src) {
    DiskEstimate e;
    if (o.hook && o.hook->disk_estimate) e = o.hook->disk_estimate();
    std::error_code ec;
    if (!e.shader_cache && !shader_src.empty() && std::filesystem::is_regular_file(widen(shader_src), ec))
        e.shader_cache = std::filesystem::file_size(widen(shader_src), ec);
    return e;
}

bool textures_wanted(const SetupOptions& o) { return !o.skip_textures && o.hook && o.hook->texture_cache; }

std::string gb(u64 b) {
    char s[32];
    std::snprintf(s, sizeof s, "%.1f GB", double(b) / 1e9);
    return s;
}

constexpr const char* kStateName = "setup_state.tsv";
constexpr const char* kStateMagic = "nx_setup_state";          // first line of the state file: "<magic>\t<version>"
constexpr const char* kStateMagicLegacy = "mprs_setup_state";  // what the first Metroid release wrote; read the same way

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
    void set_meta(const std::string& k, const std::string& v) {   // an empty value removes the key
        std::lock_guard lk(mu_);
        auto it = meta.find(k);
        if (v.empty()) {
            if (it != meta.end()) { meta.erase(it); dirty_ = true; }
        } else if (it == meta.end() || it->second != v) {
            meta[k] = v;
            dirty_ = true;
        }
    }
    void clear_files() {
        std::lock_guard lk(mu_);
        files_.clear();
        dirty_ = true;
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
            text = std::string(kStateMagic) + "\t1\n";
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
                if (f.size() < 2 || (f[0] != kStateMagic && f[0] != kStateMagicLegacy) || f[1] != "1") return;
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

struct OpenedAoc {
    u64 title_id = 0;
    u32 version = 0;
    std::string dir;                        // "aoc/<16 hex>"
    std::unique_ptr<Romfs> rom;
};

struct Opened {
    KeySet keys;
    std::vector<std::unique_ptr<Container>> c;
    DumpSet t;
    const Nca* program = nullptr;           // the NCA the ExeFS comes from (the update's when there is one)
    std::unique_ptr<Pfs> exefs;
    std::unique_ptr<Romfs> rom;
    std::vector<OpenedAoc> aoc;
    std::string build_id;
    std::string version;
    std::string update;                     // display version of the applied update, "" for none
    std::string kinds;                      // "XCI + NSP"
    std::vector<std::string> notes;         // InputSummary::notes
};

struct Supported {
    std::string build_id;                   // upper case, 40 digits
    std::string version;
};

std::string norm_build(std::string b) {
    for (char& c : b) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    while (b.size() < 40) b += '0';
    return b;
}

std::vector<Supported> supported_builds(const SetupOptions& o) {
    std::vector<Supported> v;
    for (const GameBuild& b : o.profile.builds) v.push_back({norm_build(b.build_id), b.version ? b.version : ""});
    return v;
}

std::string supported_text(const std::vector<Supported>& v) {
    std::string s;
    for (const Supported& x : v) {
        if (!s.empty()) s += ", ";
        s += (x.version.empty() ? std::string("build ") : "v" + x.version + " (build ") + x.build_id + (x.version.empty() ? "" : ")");
    }
    return s;
}

// The display version from the control NCA's control.nacp (offset 0x3060, 16 bytes), or "".
std::string display_version(const Nca* control) {
    if (!control) return "";
    try {
        const int rs = control->romfs_section();
        if (rs < 0) return "";
        auto rom = Romfs::open(control->open_romfs_image(rs));
        for (const RomfsFile& f : rom->files()) {
            if (f.path != "control.nacp" || f.size < 0x3070) continue;
            char v[17] = {};
            rom->open_file(f)->read(0x3060, v, 16);
            std::string s(v);
            if (s.find_first_not_of("0123456789.") != std::string::npos) return "";
            return s;
        }
    } catch (const std::exception& e) {
        NXS_LOGW("Could not read the version from the dump: %s", e.what());
    }
    return "";
}

std::string main_build_id(const Pfs& exefs) {
    const PfsEntry* m = exefs.find("main");
    if (!m) fail(Fail::Input, "The dump's game program is incomplete (\"main\" is missing).");
    return nso_read_info(*exefs.open_file(*m)).build_id();
}

void load_keys(const SetupOptions& o, KeySet& keys) {
    std::vector<std::string> files = o.key_files;
    if (files.empty()) {
        auto found = find_key_files();
        if (found.empty())
            fail(Fail::Keys, "No Switch keys file was found. Pick your prod.keys file (dumped from your own Switch with "
                             "Lockpick_RCM), or put it next to this program.");
        files.push_back(found.front().path);
        NXS_LOGI("Using the keys found in the %s folder.", found.front().source.c_str());
        std::string tk = sibling_title_keys(found.front().path);
        if (!tk.empty()) files.push_back(tk);
    }
    for (const std::string& f : files) keys.load_file(f);
    NXS_LOGI("Keys loaded: %s", keys.describe().c_str());
}

std::unique_ptr<Opened> open_dump(const SetupOptions& o) {
    auto d = std::make_unique<Opened>();
    load_keys(o, d->keys);
    const std::vector<std::string> inputs = setup_inputs(o);
    if (inputs.empty()) fail(Fail::Input, "No game dump was chosen.");
    std::vector<const Container*> cs;
    for (const std::string& in : inputs) {
        d->c.push_back(open_container(in));
        cs.push_back(d->c.back().get());
        if (!d->kinds.empty()) d->kinds += " + ";
        d->kinds += d->c.back()->kind;
        NXS_LOGI("Dump: %s, %s, %zu archives", in.c_str(), d->c.back()->kind.c_str(), d->c.back()->ncas.size());
    }
    d->t = select_titles(cs, d->keys, o.profile.title_id, o.profile.title);
    const Nca* base = d->t.base.get();
    const Nca* patch = d->t.patch.get();
    for (u32 v : d->t.other_patches) NXS_LOGI("An older update (v%u) was also given; the newest one is used.", v);
    for (const std::string& m : d->t.skipped) d->notes.push_back(m);
    for (size_t i = 0; i < inputs.size(); ++i)
        if (!d->t.input_used[i]) {
            d->notes.push_back("Nothing in " + inputs[i] + " belongs to this game; the file was not used.");
            NXS_LOGW("%s", d->notes.back().c_str());
        }
    const std::string shown = display_version(d->t.control.get());
    if (patch) d->update = shown.empty() ? "v" + std::to_string(d->t.patch_version) : shown;

    d->program = patch ? patch : base;
    d->exefs = d->program->open_pfs0(0);
    for (const char* need : {"main", "rtld", "sdk", "subsdk0", "main.npdm"})
        if (!d->exefs->find(need)) fail(Fail::Input, "The dump's game program is incomplete (\"%s\" is missing).", need);
    d->build_id = main_build_id(*d->exefs);
    const std::vector<Supported> sup = supported_builds(o);
    const Supported* match = nullptr;
    for (const Supported& x : sup)
        if (x.build_id == norm_build(d->build_id)) match = &x;
    if (!match) {
        std::string found = !shown.empty() ? "v" + shown : patch ? "update v" + std::to_string(d->t.patch_version) : "the base game";
        std::string hint;
        if (patch) {
            try {
                const std::string bb = norm_build(main_build_id(*base->open_pfs0(0)));
                for (const Supported& x : sup)
                    if (x.build_id == bb) hint = " The base game alone is supported: leave the update out.";
            } catch (const Error&) {
            }
        }
        fail(Fail::Input, "This version of the game is not supported: found %s (program build id %s). Supported: %s.%s",
             found.c_str(), d->build_id.c_str(), supported_text(sup).c_str(), hint.c_str());
    }
    d->version = !shown.empty() ? shown : (d->t.have_version || patch) ? match->version : "";

    const int rs = base->romfs_section();
    if (rs < 0) fail(Fail::Input, "The dump has no game data (RomFS) section.");
    if (patch) {
        const int prs = patch->romfs_section();
        if (prs < 0) fail(Fail::Input, "The update has no game data (RomFS) section.");
        d->rom = Romfs::open(patch->open_romfs_image(prs, base));
    } else {
        d->rom = Romfs::open(base->open_romfs_image(rs));
    }
    NXS_LOGI("Game %s version %s%s%s, program %s, %zu files (%s)", o.profile.title_id, d->version.c_str(),
              patch ? ", update " : "", d->update.c_str(), d->build_id.c_str(), d->rom->files().size(),
              gb(d->rom->total_file_bytes()).c_str());

    for (AocContent& a : d->t.aoc) {
        try {
            const int ars = a.data->romfs_section();
            if (ars < 0) fail(Fail::Input, "no RomFS section");
            char dir[32];
            std::snprintf(dir, sizeof dir, "aoc/%016llX", (unsigned long long)a.title_id);
            OpenedAoc oa{a.title_id, a.version, dir, Romfs::open(a.data->open_romfs_image(ars))};
            NXS_LOGI("DLC %s v%u: %zu files (%s)", dir + 4, a.version, oa.rom->files().size(), gb(oa.rom->total_file_bytes()).c_str());
            d->aoc.push_back(std::move(oa));
        } catch (const Error& e) {
            if (e.kind == Fail::Cancelled) throw;
            if (e.kind == Fail::Keys)
                fail(Fail::Keys, "DLC %016llX cannot be opened: %s", (unsigned long long)a.title_id, e.what());
            char m[64];
            std::snprintf(m, sizeof m, "DLC %016llX cannot be read and is skipped: ", (unsigned long long)a.title_id);
            d->notes.push_back(m + std::string(e.what()));
            NXS_LOGW("%s", d->notes.back().c_str());
        }
    }
    return d;
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

// Relative output path, '/' separated and lower case (Windows names do not differ by case).
std::string lower_rel(std::string s) {
    for (char& c : s) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Deletes every file under out/<sub> (for each sub) whose relative path is not in keep, then the folders left empty. Returns the
// number of files removed.
int remove_files_not_in(const std::string& out, const std::vector<std::string>& subs, const std::set<std::string>& keep) {
    namespace fs = std::filesystem;
    int n = 0;
    const fs::path root(widen(out));
    for (const std::string& sub : subs) {
        std::error_code ec;
        const fs::path top = root / widen(sub);
        if (!fs::is_directory(top, ec)) continue;
        std::vector<fs::path> dirs;
        for (fs::recursive_directory_iterator it(top, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec)) { dirs.push_back(it->path()); continue; }
            const std::string rel = lower_rel(narrow(it->path().lexically_relative(root).wstring()));
            if (!keep.count(rel) && fs::remove(it->path(), ec)) ++n;
        }
        std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
        for (const fs::path& d : dirs)
            if (fs::is_empty(d, ec)) fs::remove(d, ec);
        if (fs::is_empty(top, ec)) fs::remove(top, ec);
    }
    return n;
}

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
    if (*o.profile.shader_pack) {
        std::string p = path_join(exe_dir(), o.profile.shader_pack);
        if (path_exists(p)) return p;
    }
    return "";
}

// True when an earlier run in out_dir finished with the shaders installed: its state says finished, its game ini has the shader_cache
// line that run wrote, and results.tsv is on disk. Setup then keeps those shaders when no pack is given.
bool shader_install_present(const SetupOptions& o, State& st) {
    if (o.out_dir.empty() || st.get_meta("finished") != "1") return false;
    std::string ini;
    if (!read_text_file(path_join(o.out_dir, o.profile.game_ini), ini) || ini.find("shader_cache = shader_cache/spirv") == std::string::npos)
        return false;
    return path_exists(path_join(path_join(o.out_dir, "shader_cache\\spirv"), "results.tsv"));
}

bool shaders_wanted(const SetupOptions& o) { return *o.profile.shader_pack || !o.shader_cache_src.empty(); }

bool shaders_available(const SetupOptions& o, State& st) {
    return !shaders_wanted(o) || !find_shader_source(o).empty() || shader_install_present(o, st) || o.allow_no_shaders;
}

std::string shader_pack_missing_text(const SetupOptions& o) {
    return std::string("Shader pack not found. Put ") + o.profile.shader_pack + " in the same folder as the setup program (" + exe_dir() +
           "), or give its path with --shader-pack. The game cannot draw anything without it.";
}

std::string make_ini(const SetupOptions& o, int quality, bool with_shaders) {
    std::string s =
        "; Written by " NXS_EXE_NAME ". Setup rewrites this file only while this first line is present; delete the line to keep your edits.\n"
        "[fs]\nromfs = romfs\n"
        "[nvn]\nbc7_cache = bc7_cache\nbc7_quality = " + std::to_string(quality) + "\n";
    if (with_shaders) s += "shader_cache = shader_cache/spirv\n";
    if (o.hook && o.hook->ini_lines)
        for (const std::string& l : o.hook->ini_lines()) s += l + "\n";
    return s;
}

// Reads and checks programs.bin; false when it is missing or not valid.
bool read_programs_file(const std::string& path, std::vector<nxfmt::ProgramHashes>& out) {
    try {
        StoragePtr f = open_file_storage(path);
        const std::vector<u8> buf = read_all(*f, 0, static_cast<size_t>(f->size()));
        return nxfmt::programs_decode(buf.data(), buf.size(), out) == nullptr;
    } catch (const Error&) {
        return false;
    }
}

// The program list of the game's hook, as shader_cache/programs.bin: every shader program of the player's own game, for the runtime to
// compile ahead of play. Called only after the shader cache was installed, and only for a game whose hook lists programs. A failed scan is
// logged and does not fail the setup; a cancel is passed on. Skipped when a valid list with programs is already there (unless
// verify_output asks for a rescan).
void write_program_list(const SetupOptions& o, const std::function<bool()>& cancelled) {
    if (!o.hook || !o.hook->program_list) return;
    const std::string path = to_win(path_join(o.out_dir, "shader_cache\\programs.bin"));
    if (!o.verify_output) {
        std::vector<nxfmt::ProgramHashes> have;
        if (read_programs_file(path, have) && !have.empty()) {
            NXS_LOGI("Shader program list already present (%zu programs).", have.size());
            return;
        }
    }
    const auto t0 = Clock::now();
    try {
        const ProgramList pl = o.hook->program_list(path_join(o.out_dir, "romfs"), o.threads, cancelled);
        const std::vector<u8> bytes = nxfmt::programs_encode(pl.programs);
        AtomicFileWriter w(path);
        w.write(bytes.data(), bytes.size());
        w.finish();
        NXS_LOGI("Shader programs: %zu listed (%s) in %.1f s.", pl.programs.size(), pl.summary.c_str(), secs_since(t0));
    } catch (const Error& e) {
        if (e.kind == Fail::Cancelled) throw;
        NXS_LOGW("Shader program list not written: %s", e.what());
    } catch (const std::exception& e) {
        NXS_LOGW("Shader program list not written: %s", e.what());
    }
}

}  // namespace

std::vector<std::string> setup_inputs(const SetupOptions& o) {
    std::vector<std::string> v;
    if (!o.input_path.empty()) v.push_back(o.input_path);
    for (const std::string& p : o.input_paths)
        if (!p.empty()) v.push_back(p);
    return v;
}

InputSummary inspect_input(const SetupOptions& o) {
    InputSummary s;
    try {
        auto d = open_dump(o);
        s.container = d->kinds;
        s.title_id = o.profile.title_id;
        s.version = d->version;
        s.build_id = d->build_id;
        s.update = d->update;
        s.dlc_count = static_cast<u32>(d->aoc.size());
        for (auto& a : d->aoc) s.dlc_bytes += a.rom->total_file_bytes();
        s.notes = d->notes;
        s.romfs_files = static_cast<u32>(d->rom->files().size());
        s.romfs_bytes = d->rom->total_file_bytes();
        for (auto& e : d->exefs->entries()) s.exefs_bytes += e.size;
        s.bc7_bytes_estimate = disk_estimate_of(o, std::string()).texture_cache;
        State st(path_join(o.out_dir, kStateName));
        u64 need = 0;
        for (auto& e : d->exefs->entries()) need += bytes_missing(path_join(o.out_dir, "exefs"), e.name, e.size);
        for (auto& f : d->rom->files()) need += bytes_missing(path_join(o.out_dir, "romfs"), f.path, f.size);
        for (auto& a : d->aoc)
            for (auto& f : a.rom->files()) need += bytes_missing(path_join(o.out_dir, a.dir), f.path, f.size);
        {
            const std::string pack = find_shader_source(o);
            const DiskEstimate est = disk_estimate_of(o, pack);
            if (textures_wanted(o) && st.get_meta("textures_done").empty()) need += est.texture_cache;
            if (!pack.empty()) need += est.shader_cache;
        }
        s.disk_needed = need + need / 50;
        s.disk_free = o.out_dir.empty() ? 0 : free_disk_bytes(o.out_dir);
        s.supported = true;
        if (s.disk_free && s.disk_free < s.disk_needed) {
            s.supported = false;
            s.problem = "Not enough free space: setup needs about " + gb(s.disk_needed) + " and the drive has " + gb(s.disk_free) +
                        " free.";
        } else if (!shaders_available(o, st)) {
            s.supported = false;
            s.problem = shader_pack_missing_text(o);
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
    NXS_LOGI("---- setup started (quality %d, threads %d) ----", o.bc7_quality, o.threads);
    auto cancelled = [&] { return r.cancel_requested(); };
    auto check_cancel = [&] { if (cancelled()) fail(Fail::Cancelled, "Stopped by the user. Run setup again to continue where it left off."); };
    State st(path_join(o.out_dir, kStateName));
    for (const char* sub : {"exefs", "romfs", "aoc", "bc7_cache", "shader_cache"})
        if (int n = remove_part_files(path_join(o.out_dir, sub))) NXS_LOGI("Removed %d unfinished file(s) from an earlier run in %s.", n, sub);
    auto begin = [&](Stage s, u64 total, const char* unit) {
        NXS_LOGI("%s", stage_name(s));
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
            NXS_LOGI("Checking the dump's own hashes (this reads the whole file once).");
            struct Check { const Nca* n; int sec; const Nca* base; };
            std::vector<Check> checks{{d->program, 0, nullptr}};
            if (d->t.patch) checks.push_back({d->t.patch.get(), d->t.patch->romfs_section(), d->t.base.get()});
            else checks.push_back({d->t.base.get(), d->t.base->romfs_section(), nullptr});
            for (const AocContent& a : d->t.aoc)
                if (std::any_of(d->aoc.begin(), d->aoc.end(), [&](const OpenedAoc& x) { return x.title_id == a.title_id; }))
                    checks.push_back({a.data.get(), a.data->romfs_section(), nullptr});
            u64 total = 0;
            for (const Check& c : checks)
                if (const NcaSection* s = c.n->section(c.sec)) total += s->size;
            r.stage_begin(Stage::Inspect, stage_name(Stage::Inspect), total, "bytes");
            u64 base = 0;
            for (const Check& c : checks) {
                const NcaSection* s = c.n->section(c.sec);
                if (!s) continue;
                u64 b0 = base;
                c.n->verify_section(c.sec, [&](u64 done, u64 tot) { r.stage_progress(b0 + done * s->size / std::max<u64>(tot, 1)); },
                                    cancelled, c.base);
                base += s->size;
            }
            NXS_LOGI("The dump's hashes are all correct.");
        }
        end(Stage::Inspect, t0);
        if (o.inspect_only) { rep.total_seconds = secs_since(t_all); return rep; }
        check_cancel();

        std::string aoc_meta;
        for (auto& a : d->aoc) aoc_meta += (aoc_meta.empty() ? "" : ",") + a.dir.substr(4) + ":" + std::to_string(a.version);
        // The folder holds another version (other build, update or DLC set): drop the file records so every output is hashed
        // against the new dump or written again, remove files the new version does not have, and redo the texture cache and the
        // shader program list. "rebuild" stays set until a run finishes, so a stopped rebuild goes on as a rebuild.
        {
            const std::string old_build = st.get_meta("build_id");
            if (!old_build.empty() &&
                (old_build != d->build_id || st.get_meta("title_id") != o.profile.title_id || st.get_meta("update") != d->update ||
                 st.get_meta("aoc") != aoc_meta)) {
                NXS_LOGI("The folder holds another version of the game (build %s%s%s); every file is checked again and files the "
                          "new version does not have are removed.", old_build.c_str(), st.get_meta("update").empty() ? "" : ", update ",
                          st.get_meta("update").c_str());
                st.clear_files();
                st.set_meta("textures_done", "");
                st.set_meta("rebuild", "1");
                std::error_code ec;
                std::filesystem::remove(widen(to_win(path_join(o.out_dir, "shader_cache\\programs.bin"))), ec);
            }
        }
        const bool rebuild = st.get_meta("rebuild") == "1";
        const bool verify_out = o.verify_output || rebuild;
        st.set_meta("title_id", o.profile.title_id);
        st.set_meta("build_id", d->build_id);
        st.set_meta("update", d->update);
        st.set_meta("aoc", aoc_meta);
        st.save();

        // ---- Space -----------------------------------------------------------------------------------------------------
        t0 = begin(Stage::Space, 0, "");
        {
            if (!shaders_available(o, st)) fail(Fail::Input, "%s", shader_pack_missing_text(o).c_str());
            const std::string exedir = path_join(o.out_dir, "exefs"), romdir = path_join(o.out_dir, "romfs");
            u64 need = 0;
            for (auto& e : d->exefs->entries()) need += bytes_missing(exedir, e.name, e.size);
            for (auto& f : d->rom->files()) need += bytes_missing(romdir, f.path, f.size);
            for (auto& a : d->aoc)
                for (auto& f : a.rom->files()) need += bytes_missing(path_join(o.out_dir, a.dir), f.path, f.size);
            {
                const std::string pack = find_shader_source(o);
                const DiskEstimate est = disk_estimate_of(o, pack);
                if (textures_wanted(o) && st.get_meta("textures_done").empty()) need += est.texture_cache;
                if (!pack.empty()) need += est.shader_cache;
            }
            need += need / 50 + (64u << 20);
            u64 freeb = free_disk_bytes(o.out_dir);
            NXS_LOGI("Disk space: need about %s, free %s.", gb(need).c_str(), gb(freeb).c_str());
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
                if (file_is_done(dest, rel, e.size, st, verify_out, src.get(), cancelled, sha)) {
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
            struct Job { const Romfs* fs; const RomfsFile* f; const std::string* dir; };
            const std::string romfs_dir = "romfs";
            std::vector<Job> order;
            u64 total = d->rom->total_file_bytes();
            for (auto& f : d->rom->files()) order.push_back({d->rom.get(), &f, &romfs_dir});
            std::sort(order.begin(), order.end(), [](const Job& a, const Job& b) { return a.f->offset < b.f->offset; });
            create_dirs(path_join(o.out_dir, "romfs"));
            for (const std::string& dir : d->rom->dirs()) create_dirs(to_win(path_join(path_join(o.out_dir, "romfs"), dir)));
            for (auto& a : d->aoc) {
                const size_t first = order.size();
                for (auto& f : a.rom->files()) order.push_back({a.rom.get(), &f, &a.dir});
                std::sort(order.begin() + first, order.end(), [](const Job& x, const Job& y) { return x.f->offset < y.f->offset; });
                total += a.rom->total_file_bytes();
                const std::string top = to_win(path_join(o.out_dir, a.dir));
                create_dirs(top);
                for (const std::string& dir : a.rom->dirs()) create_dirs(to_win(path_join(top, dir)));
            }
            t0 = begin(Stage::Romfs, total, "bytes");
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
                        const RomfsFile& f = *order[i].f;
                        const std::string rel = *order[i].dir + "/" + f.path;
                        const std::string dest = to_win(path_join(o.out_dir, rel));
                        StoragePtr src = order[i].fs->open_file(f);
                        std::string sha;
                        if (file_is_done(dest, rel, f.size, st, verify_out, src.get(), [&] { return stop.load() || cancelled(); }, sha)) {
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
            NXS_LOGI("RomFS: %llu files written, %llu already present.", (unsigned long long)wfiles.load(), (unsigned long long)sfiles.load());
            if (rebuild) {
                std::set<std::string> keep;
                for (auto& e : d->exefs->entries()) keep.insert(lower_rel("exefs/" + e.name));
                for (const Job& j : order) keep.insert(lower_rel(*j.dir + "/" + j.f->path));
                const int n = remove_files_not_in(o.out_dir, {"exefs", "romfs", "aoc"}, keep);
                if (n) NXS_LOGI("Removed %d file(s) the new version does not have.", n);
            }
        }
        check_cancel();

        // ---- Textures --------------------------------------------------------------------------------------------------
        if (textures_wanted(o)) {
            t0 = begin(Stage::Textures, 0, "textures");
            const std::string cache_dir = path_join(o.out_dir, "bc7_cache");
            std::string prev = st.get_meta("textures_done");   // "<quality>:<converted+skipped>"
            // A cache made by another revision of the converter is thrown away (its files would pass the size check).
            const std::string marker = path_join(cache_dir, "encoder.txt");
            const std::string want_marker = "bc7 converter revision " + std::to_string(kTextureEncoderRev) + "\n";
            {
                std::string have;
                read_text_file(marker, have);
                if (have != want_marker && path_exists(cache_dir)) {
                    NXS_LOGI("The texture cache was made by an older converter: rebuilding it.");
                    std::error_code ec;
                    for (std::filesystem::directory_iterator it(widen(cache_dir), ec), end; !ec && it != end; it.increment(ec))
                        if (it->is_directory()) std::filesystem::remove_all(it->path(), ec);
                    prev.clear();
                    st.set_meta("textures_done", "");
                }
            }
            const std::string want_prefix = std::to_string(o.bc7_quality) + ":";
            if (!o.verify_output && !prev.empty() && prev.compare(0, want_prefix.size(), want_prefix) == 0) {
                NXS_LOGI("Texture cache already built (%s textures at quality %d).", prev.substr(want_prefix.size()).c_str(), o.bc7_quality);
                ++rep.textures_skipped;
            } else {
                TexCacheOptions to;
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
                TextureList list = o.hook->texture_cache(path_join(o.out_dir, "romfs"), cancelled);
                TexCacheStats ts = build_texture_cache(to, std::move(list.textures), [](u64, u64) {}, cancelled);
                ts.failed += list.failed;
                rep.textures_total = ts.textures_total;
                rep.textures_converted = ts.converted;
                rep.textures_skipped = ts.skipped;
                rep.textures_failed = ts.failed;
                rep.bytes_written += ts.bytes_written;
                NXS_LOGI("Textures: %u total, %u converted, %u already done, %u failed, %s written, %.0f s.", ts.textures_total,
                          ts.converted, ts.skipped, ts.failed, gb(ts.bytes_written).c_str(), ts.seconds);
                if (ts.failed)
                    fail(Fail::Generic, "%u textures could not be converted (see setup.log). Run setup again; if it keeps failing "
                                        "the dump may be damaged.", ts.failed);
                write_text_file_atomic(marker, want_marker);
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
            if (src.empty() && shader_install_present(o, st)) {
                NXS_LOGI("No shader pack given; keeping the shaders already installed in shader_cache\\spirv.");
                shaders = true;
            } else if (src.empty() && !shaders_wanted(o)) {
                NXS_LOGI("This game ships no shader pack; no shaders to install.");
            } else if (src.empty()) {
                if (!o.allow_no_shaders) fail(Fail::Input, "%s", shader_pack_missing_text(o).c_str());
                NXS_LOGW("Skipping the shader pack (--no-shaders). The game will not draw anything until shaders are installed.");
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
                NXS_LOGI("Shaders: %u files, %u installed, %u already present.", sr.files_total, sr.files_installed, sr.files_skipped);
            }
            if (shaders) write_program_list(o, cancelled);
            end(Stage::ShaderCache, t0);
        }

        // ---- Finish ----------------------------------------------------------------------------------------------------
        t0 = begin(Stage::Finish, 0, "");
        {
            const std::string ini_path = path_join(o.out_dir, o.profile.game_ini);
            std::string existing;
            bool ours = !read_text_file(ini_path, existing) || existing.rfind("; Written by " NXS_EXE_NAME, 0) == 0;
            if (ours) write_text_file_atomic(ini_path, make_ini(o, o.bc7_quality, shaders));
            else NXS_LOGW("%s was edited by hand; left alone. It must contain bc7_quality = %d.", o.profile.game_ini, o.bc7_quality);
            st.set_meta("bc7_quality", std::to_string(o.bc7_quality));
            st.set_meta("finished", "1");
            st.set_meta("rebuild", "");
            st.save(true);
        }
        end(Stage::Finish, t0);
        rep.total_seconds = secs_since(t_all);
        NXS_LOGI("---- setup finished in %.0f s ----", rep.total_seconds);
        return rep;
    } catch (const Error& e) {
        st.save();
        NXS_LOGE("Setup stopped: %s", e.what());
        throw;
    } catch (const std::exception& e) {
        st.save();
        NXS_LOGE("Setup failed: %s", e.what());
        throw Error(Fail::Generic, e.what());
    }
}

}  // namespace nxs
