// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end tests of inspect_input and run_setup (src/pipeline.cpp) on synthetic dumps from tests/synth_nca.cpp.
// Every case writes its dump, keys and output under a scratch folder in the temp directory, removed at the end of the case.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "check.h"
#include "common.h"
#include "fileio.h"
#include "keys.h"
#include "log.h"
#include "pfs.h"
#include "pipeline.h"
#include "storage.h"
#include "synth.h"
#include "synth_nca.h"

using namespace mprs;
namespace fs = std::filesystem;
using mprs_test::SynthCorruption;
using mprs_test::SynthDump;
using mprs_test::SynthFiles;
using mprs_test::SynthKeys;
using mprs_test::SynthNcaSpec;
using mprs_test::SynthSection;

namespace {

constexpr uint64_t kProgramId = 0x010012101468C000ull;
const char* const kTitleId = "010012101468C000";
const char* const kBuildId = "0123456789ABCDEF0123456789ABCDEF01234567";
const char* const kOtherBuildId = "FEDCBA9876543210FEDCBA9876543210FEDCBA98";

// Stage sequences (one entry per stage, consecutive repeats removed by distinct_runs).
const std::vector<Stage> kOrderNoTextures = {Stage::Inspect, Stage::Space, Stage::Exefs, Stage::Romfs, Stage::ShaderCache,
                                             Stage::Finish};
const std::vector<Stage> kOrderWithTextures = {Stage::Inspect, Stage::Space, Stage::Exefs, Stage::Romfs, Stage::Textures,
                                               Stage::ShaderCache, Stage::Finish};
const std::vector<Stage> kOnlyInspect = {Stage::Inspect};

// ---- scratch folders ---------------------------------------------------------------------------------------------------------
// A folder under the temp directory, emptied on creation and removed when the object goes away. The log file is closed first
// (run_setup leaves setup.log open), so the folder can be deleted on Windows.
class Scratch {
public:
    explicit Scratch(const std::string& name) : dir_(fs::temp_directory_path() / ("mprs_test_pipeline_" + name)) {
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~Scratch() {
        log_close();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    std::string path(const std::string& rel = "") const { return rel.empty() ? dir_.string() : (dir_ / rel).string(); }

private:
    fs::path dir_;
};

// ---- file helpers ------------------------------------------------------------------------------------------------------------
std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 5 + (i >> 9));
    return v;
}

std::vector<uint8_t> to_bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
std::string as_text(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

std::vector<uint8_t> read_bytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamoff n = f.tellg();
    std::vector<uint8_t> v(static_cast<size_t>(n));
    f.seekg(0);
    if (n > 0) f.read(reinterpret_cast<char*>(v.data()), n);
    return v;
}

void write_bytes(const std::string& p, const std::vector<uint8_t>& v) {
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
}

bool same_bytes(const std::string& p, const std::vector<uint8_t>& want) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && read_bytes(p) == want;
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// Regular files under dir (0 when dir does not exist). ext_only = count only names ending with this suffix.
size_t count_files(const std::string& dir, const std::string& suffix = "") {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    size_t n = 0;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string name = it->path().filename().string();
        if (suffix.empty() || (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0))
            ++n;
    }
    return n;
}

// Regular files under dir other than the log and the state file: must be zero after a refusal.
size_t files_other_than_log(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    size_t n = 0;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string name = it->path().filename().string();
        if (name != "setup.log" && name != "setup_state.tsv") ++n;
    }
    return n;
}

// Every file under a, by relative path, compared with b (same names, same bytes, same count). setup.log is not compared.
bool trees_equal(const std::string& a, const std::string& b) {
    std::map<std::string, std::vector<uint8_t>> ta, tb;
    for (const std::string* root : {&a, &b}) {
        auto& t = (root == &a) ? ta : tb;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(*root, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            if (it->path().filename() == "setup.log") continue;
            t[fs::relative(it->path(), *root, e2).string()] = read_bytes(it->path().string());
        }
    }
    return ta == tb && !ta.empty();
}

uint64_t total_bytes(const SynthFiles& files) {
    uint64_t n = 0;
    for (const auto& f : files) n += f.second.size();
    return n;
}

// ---- test data ---------------------------------------------------------------------------------------------------------------
SynthFiles romfs_files() {
    return {
        {"Worlds/MP1/Test.pak", pattern(3000, 9)},
        {"Worlds/MP1/Sub/Deep/Other.bin", pattern(70000, 3)},
        {"Audio/ding.bin", pattern(20000, 2)},
        {"readme.txt", to_bytes("ok")},
    };
}

// 40 files of 1 MiB each, for the resume test.
SynthFiles big_romfs_files() {
    SynthFiles v;
    for (int i = 0; i < 40; ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "Bulk/part%02d.bin", i);
        v.push_back({name, pattern(size_t(1) << 20, uint8_t(i + 1))});
    }
    return v;
}

// Makes a dump of the program with the given section damage (RomFS section = 1 in the spec below).
// Returns the dump path; the standard keys file is written next to it and its path is returned in keys_out.
std::string make_damaged_dump(const std::string& dir, const SynthFiles& files, const SynthCorruption& c, std::string& keys_out) {
    SynthNcaSpec sp;
    sp.content_type = 0;
    sp.program_id = kProgramId;
    sp.content_index = 0;
    sp.key_generation = 11;
    sp.sections = {SynthSection{1, 3, mprs_test::synth_exefs(kBuildId)}, SynthSection{0, 3, mprs_test::synth_romfs(files)}};
    sp.corrupt = {c};
    SynthFiles entries = {{"010012101468C000.nca", mprs_test::synth_nca(sp)},
                          {"010012101468C000.cnmt.nca", mprs_test::synth_cnmt_nca(kProgramId, 0)}};
    const std::string path = (fs::path(dir) / "game.nsp").string();
    write_bytes(path, mprs_test::synth_nsp(entries));
    keys_out = mprs_test::write_synth_keys_file(dir);
    return path;
}

// The four files of the shader cache tests: results.tsv and three cache files.
SynthFiles shader_files() {
    return {
        {"results.tsv", to_bytes("hash\tname\n0123456789abcdef\tshader\n")},
        {"0123456789abcdef.spv", pattern(4096, 11)},
        {"00004b0d6e7742b4.spv", pattern(1000, 12)},
        {"fedcba9876543210.spv", pattern(64, 13)},
    };
}

// The same four files as one shader pack (MPRSHC01, every entry stored; layout as in src/shadercache.h), as bytes.
std::vector<uint8_t> make_shader_pack_bytes() {
    const SynthFiles items = shader_files();
    auto put = [](std::vector<uint8_t>& v, uint64_t x, int n) {
        for (int i = 0; i < n; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
    };
    std::vector<uint8_t> out = {'M', 'P', 'R', 'S', 'H', 'C', '0', '1'};
    put(out, items.size(), 4);
    put(out, 0, 4);
    uint64_t table = 0;
    for (const auto& item : items) table += 2 + item.first.size() + 8 + 8 + 8 + 32 + 1;
    uint64_t off = 16 + table;
    std::vector<uint8_t> payload;
    for (const auto& item : items) {
        const std::vector<uint8_t>& data = item.second;
        put(out, item.first.size(), 2);
        out.insert(out.end(), item.first.begin(), item.first.end());
        put(out, off, 8);             // data offset
        put(out, data.size(), 8);     // stored size
        put(out, data.size(), 8);     // raw size
        const auto h = mprs_test::synth_sha256(data.data(), data.size());
        out.insert(out.end(), h.begin(), h.end());
        out.push_back(0);             // method 0 = stored
        off += data.size();
        payload.insert(payload.end(), data.begin(), data.end());
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// A shader cache folder: results.tsv, three cache files, and a manifest.sha256 listing all four.
std::string make_shader_folder(const std::string& dir) {
    fs::create_directories(dir);
    const SynthFiles items = shader_files();
    std::string manifest;
    for (const auto& [name, data] : items) {
        const auto h = mprs_test::synth_sha256(data.data(), data.size());
        manifest += hex_encode(h.data(), 32) + "  " + name + "\n";
        write_bytes((fs::path(dir) / name).string(), data);
    }
    write_bytes((fs::path(dir) / "manifest.sha256").string(), to_bytes(manifest));
    return dir;
}

// ---- reporter ----------------------------------------------------------------------------------------------------------------
// Records every stage_begin / stage_end in call order and the progress per stage. Can request cancel: at the start of stage
// cancel_in when cancel_after == 0, or after cancel_after stage_progress calls while cancel_in is the current stage.
class RecordingReporter : public Reporter {
public:
    std::vector<Stage> begins, ends;
    std::optional<Stage> cancel_in;
    u64 cancel_after = 0;

    void stage_begin(Stage s, const std::string&, u64 total, const char*) override {
        std::lock_guard lk(mu_);
        begins.push_back(s);
        current_ = s;
        total_[key(s)] = total;
        if (cancel_in && *cancel_in == s && cancel_after == 0) cancel_.store(true);
    }
    void stage_progress(u64 done) override {
        std::lock_guard lk(mu_);
        const int k = key(current_);
        ++calls_[k];
        last_[k] = done;
        peak_[k] = std::max(peak_[k], done);
        if (cancel_in && *cancel_in == current_ && cancel_after > 0 && calls_[k] >= cancel_after) cancel_.store(true);
    }
    void stage_end(Stage s, double) override {
        std::lock_guard lk(mu_);
        ends.push_back(s);
    }
    bool cancel_requested() override { return cancel_.load(); }

    u64 total_of(Stage s) { std::lock_guard lk(mu_); return total_[key(s)]; }
    u64 peak_of(Stage s) { std::lock_guard lk(mu_); return peak_[key(s)]; }
    u64 last_of(Stage s) { std::lock_guard lk(mu_); return last_[key(s)]; }
    u64 calls_of(Stage s) { std::lock_guard lk(mu_); return calls_[key(s)]; }

private:
    static int key(Stage s) { return static_cast<int>(s); }
    std::mutex mu_;
    std::atomic<bool> cancel_{false};
    Stage current_ = Stage::Inspect;
    std::map<int, u64> total_, last_, peak_, calls_;
};

// Consecutive repeats removed: Inspect is announced twice by run_setup (once for the stage, once for the hash check).
std::vector<Stage> distinct_runs(const std::vector<Stage>& v) {
    std::vector<Stage> out;
    for (Stage s : v)
        if (out.empty() || out.back() != s) out.push_back(s);
    return out;
}

const char* kind_name(Fail f) {
    switch (f) {
        case Fail::Generic: return "Generic";
        case Fail::Input: return "Input";
        case Fail::Keys: return "Keys";
        case Fail::Disk: return "Disk";
        case Fail::Cancelled: return "Cancelled";
    }
    return "?";
}

// ---- running -----------------------------------------------------------------------------------------------------------------
struct Outcome {
    bool threw = false;
    bool non_error = false;   // threw something that is not mprs::Error
    Fail kind = Fail::Generic;
    std::string msg;
};

template <class F>
Outcome capture(F&& f) {
    Outcome o;
    try {
        f();
    } catch (const Error& e) {
        o.threw = true;
        o.kind = e.kind;
        o.msg = e.what();
    } catch (const std::exception& e) {
        o.threw = true;
        o.non_error = true;
        o.msg = e.what();
    }
    return o;
}

struct RunResult {
    Outcome out;
    SetupReport rep;
};

RunResult run(const SetupOptions& o, RecordingReporter& r) {
    RunResult rr;
    rr.out = capture([&] { rr.rep = run_setup(o, r); });
    if (rr.out.threw)
        std::printf("  note: run_setup threw %s (%s): %s\n", kind_name(rr.out.kind), rr.out.non_error ? "non-Error" : "Error",
                    rr.out.msg.c_str());
    return rr;
}

SetupOptions base_opts(const std::string& input, const std::string& keys, const std::string& build_id, const std::string& out) {
    SetupOptions o;
    o.input_path = input;
    o.key_files = {keys};
    o.out_dir = out;
    o.expected_build_id = build_id;
    o.skip_textures = true;
    o.verify_input = true;
    o.verify_output = false;
    o.allow_no_shaders = true;   // most cases have no shader pack; the shader cases set this back to false
    return o;
}

// Checks everything a complete run writes for a dump with this build id and RomFS (quality 3, no textures).
void check_installed(const std::string& out, const std::string& build_id, const SynthFiles& files) {
    const fs::path root(out);
    const auto pfs = Pfs::open(make_memory_storage(mprs_test::synth_exefs(build_id)));
    CHECK_EQ(count_files((root / "exefs").string()), pfs->entries().size());
    for (const PfsEntry& e : pfs->entries()) {
        const std::vector<uint8_t> want = read_all(*pfs->open_file(e), 0, static_cast<size_t>(e.size));
        CHECK(same_bytes((root / "exefs" / e.name).string(), want));
    }
    CHECK_EQ(count_files((root / "romfs").string()), files.size());
    for (const auto& [rel, bytes] : files) CHECK(same_bytes((root / "romfs" / rel).string(), bytes));
    const std::vector<uint8_t> ini = read_bytes((root / "mpr.ini").string());
    CHECK(has(as_text(ini), "bc7_quality = 3"));
    CHECK(has(as_text(ini), "; Written by mpr_setup"));
    CHECK(fs::is_regular_file(root / "setup_state.tsv"));
    CHECK(fs::is_regular_file(root / "setup.log"));
    std::error_code ec;
    CHECK(fs::file_size(root / "setup.log", ec) > 0);
}

// run_setup must refuse with `kind` (message containing needle, when given), write nothing but setup.log and the state, and
// inspect_input must report the same problem without throwing.
void expect_refusal(const SetupOptions& o, Fail kind, const char* needle) {
    RecordingReporter rec;
    const RunResult rr = run(o, rec);
    CHECK(rr.out.threw);
    CHECK(!rr.out.non_error);
    CHECK(rr.out.kind == kind);
    if (needle && *needle) CHECK(has(rr.out.msg, needle));
    CHECK_EQ(files_other_than_log(o.out_dir), size_t(0));

    InputSummary in;
    const Outcome io = capture([&] { in = inspect_input(o); });
    CHECK(!io.threw);
    CHECK(!in.supported);
    CHECK(!in.problem.empty());
}

// ---- case 1 and 2: a complete install of an NSP (with and without ticket) or an XCI -------------------------------------------
void case_install(const char* tag, const char* kind, bool use_ticket) {
    Scratch s(tag);
    const SynthFiles files = romfs_files();
    const SynthDump d = mprs_test::make_synth_dump(kind, kProgramId, kBuildId, files, use_ticket, s.path("dump"));
    const std::string out = s.path("out");
    const SetupOptions o = base_opts(d.path, d.keys_file_path, d.build_id, out);

    const InputSummary in = inspect_input(o);
    CHECK(in.supported);
    if (!in.supported) std::printf("  inspect problem: %s\n", in.problem.c_str());
    CHECK_EQ(in.container, std::string(std::string(kind) == "nsp" ? "NSP" : "XCI"));
    CHECK_EQ(in.title_id, std::string(kTitleId));
    CHECK_EQ(in.version, std::string("1.0.0"));
    CHECK_EQ(in.build_id, std::string(kBuildId));
    CHECK_EQ(in.romfs_files, static_cast<u32>(files.size()));
    CHECK_EQ(in.romfs_bytes, total_bytes(files));

    RecordingReporter rec;
    const RunResult rr = run(o, rec);
    CHECK(!rr.out.threw);
    CHECK(distinct_runs(rec.begins) == kOrderNoTextures);
    CHECK(distinct_runs(rec.ends) == kOrderNoTextures);
    CHECK_EQ(rec.total_of(Stage::Romfs), total_bytes(files));
    CHECK_EQ(rec.peak_of(Stage::Romfs), total_bytes(files));
    if (rec.last_of(Stage::Romfs) != rec.peak_of(Stage::Romfs))
        std::printf("  note: last Romfs progress (%llu) differs from the largest (%llu): calls arrive out of order\n",
                    static_cast<unsigned long long>(rec.last_of(Stage::Romfs)),
                    static_cast<unsigned long long>(rec.peak_of(Stage::Romfs)));
    CHECK_EQ(rr.rep.files_written, static_cast<u64>(5 + files.size()));
    check_installed(out, d.build_id, files);
}

// ---- case 3: idempotent re-runs, verify_output, and repair of damaged or truncated outputs -----------------------------------
void case_idempotent() {
    Scratch s("idem");
    const SynthFiles files = romfs_files();
    const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
    const std::string out = s.path("out");
    SetupOptions o = base_opts(d.path, d.keys_file_path, d.build_id, out);
    const u64 n = 5 + files.size();

    RecordingReporter r1;
    const RunResult first = run(o, r1);
    CHECK(!first.out.threw);
    CHECK_EQ(first.rep.files_written, n);

    RecordingReporter r2;
    const RunResult second = run(o, r2);
    CHECK(!second.out.threw);
    CHECK_EQ(second.rep.files_written, u64(0));
    CHECK_EQ(second.rep.files_skipped, n);

    const fs::path victim = fs::path(out) / "romfs" / "Audio" / "ding.bin";
    const fs::file_time_type mtime_before = fs::last_write_time(victim);

    o.verify_output = true;
    RecordingReporter r3;
    const RunResult verified = run(o, r3);
    CHECK(!verified.out.threw);
    CHECK_EQ(verified.rep.files_written, u64(0));
    CHECK_EQ(verified.rep.files_skipped, n);
    CHECK(fs::last_write_time(victim) == mtime_before);

    // Same size, one byte flipped: only verify_output notices.
    const fs::path flipped = fs::path(out) / "romfs" / "Worlds" / "MP1" / "Test.pak";
    std::vector<uint8_t> bytes = read_bytes(flipped.string());
    bytes[bytes.size() / 2] ^= 0x01;
    write_bytes(flipped.string(), bytes);
    RecordingReporter r4;
    const RunResult repaired = run(o, r4);
    CHECK(!repaired.out.threw);
    CHECK_EQ(repaired.rep.files_written, u64(1));
    CHECK_EQ(repaired.rep.files_skipped, n - 1);
    CHECK(same_bytes(flipped.string(), files[0].second));

    // Size changed: rewritten even without verify_output.
    const fs::path truncated = fs::path(out) / "romfs" / "Audio" / "ding.bin";
    std::vector<uint8_t> half = read_bytes(truncated.string());
    half.resize(half.size() / 2);
    write_bytes(truncated.string(), half);
    o.verify_output = false;
    RecordingReporter r5;
    const RunResult short_file = run(o, r5);
    CHECK(!short_file.out.threw);
    CHECK_EQ(short_file.rep.files_written, u64(1));
    CHECK(same_bytes(truncated.string(), files[2].second));

    check_installed(out, d.build_id, files);
}

// ---- case 4: stop in the middle of the RomFS, then resume ------------------------------------------------------------------
void case_resume() {
    Scratch s("resume");
    const SynthFiles files = big_romfs_files();
    const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
    const std::string out = s.path("out");
    SetupOptions o = base_opts(d.path, d.keys_file_path, d.build_id, out);
    o.threads = 4;

    RecordingReporter stop_me;
    stop_me.cancel_in = Stage::Romfs;
    stop_me.cancel_after = 10;
    const RunResult cancelled = run(o, stop_me);
    CHECK(cancelled.out.threw);
    CHECK(cancelled.out.kind == Fail::Cancelled);
    CHECK_EQ(count_files(out, ".part"), size_t(0));

    // A leftover partial file from a killed run must be removed by the next run.
    write_bytes((fs::path(out) / "romfs" / "x.part").string(), to_bytes("partial"));
    CHECK_EQ(count_files(out, ".part"), size_t(1));

    RecordingReporter resume;
    const RunResult second = run(o, resume);
    CHECK(!second.out.threw);
    CHECK(!fs::exists(fs::path(out) / "romfs" / "x.part"));
    CHECK_EQ(count_files(out, ".part"), size_t(0));
    CHECK(second.rep.files_skipped > 0);
    CHECK(distinct_runs(resume.ends) == kOrderNoTextures);
    check_installed(out, d.build_id, files);
}

// ---- case 5: refusals --------------------------------------------------------------------------------------------------------
void case_refusals() {
    const SynthFiles files = romfs_files();

    {   // another game
        Scratch s("refuse_title");
        const SynthDump d = mprs_test::make_synth_dump("nsp", 0x0100000000001000ull, kBuildId, files, false, s.path("dump"));
        expect_refusal(base_opts(d.path, d.keys_file_path, d.build_id, s.path("out")), Fail::Input, "different game");
    }
    {   // update only
        Scratch s("refuse_update");
        const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId + 0x800, kBuildId, files, false, s.path("dump"));
        expect_refusal(base_opts(d.path, d.keys_file_path, d.build_id, s.path("out")), Fail::Input, "update");
    }
    {   // a valid dump, but the caller expects another build
        Scratch s("refuse_build");
        const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
        expect_refusal(base_opts(d.path, d.keys_file_path, kOtherBuildId, s.path("out")), Fail::Input, "v1.0.0");
    }
    {   // RomFS hash layer damaged: caught by the hash check (verify_input), not by parsing
        Scratch s("refuse_damaged");
        SynthCorruption c;
        c.section = 1;
        c.where = SynthCorruption::Where::HashLayer;
        std::string keys;
        const std::string dump = make_damaged_dump(s.path("dump"), files, c, keys);
        const std::string out = s.path("out");
        const SetupOptions o = base_opts(dump, keys, kBuildId, out);

        RecordingReporter rec;
        const RunResult rr = run(o, rec);
        CHECK(rr.out.threw);
        CHECK(rr.out.kind == Fail::Input);
        CHECK(has(rr.out.msg, "damaged"));
        CHECK(distinct_runs(rec.begins) == kOnlyInspect);
        CHECK_EQ(files_other_than_log(out), size_t(0));

        // inspect_input is the quick check (no pass over the whole file): damage in the hash tree is found by run_setup only.
        const InputSummary in = inspect_input(o);
        CHECK(in.supported);

        // With verify_input off, the damage is in the hash layer only, so the copy goes through.
        SetupOptions lax = o;
        lax.verify_input = false;
        lax.out_dir = s.path("out_lax");
        const RunResult lax_run = run(lax, *std::make_unique<RecordingReporter>());
        CHECK(!lax_run.out.threw);
    }
    {   // truncated file
        Scratch s("refuse_trunc");
        const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
        std::vector<uint8_t> img = read_bytes(d.path);
        CHECK(img.size() > 10000);
        img.resize(img.size() - 10000);
        write_bytes(s.path("cut/game.nsp"), img);
        expect_refusal(base_opts(s.path("cut/game.nsp"), d.keys_file_path, d.build_id, s.path("out")), Fail::Input, "");
    }
    {   // keys whose header key source differs from the dump's
        Scratch s("refuse_keys");
        const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
        SynthKeys k = SynthKeys::standard();
        k.header_kek_source[0] ^= 0x5A;
        const std::string bad = mprs_test::write_synth_keys_file_with(s.path("keys"), k, "prod.keys");
        expect_refusal(base_opts(d.path, bad, d.build_id, s.path("out")), Fail::Keys, "");
    }
    {   // no keys given and none in the usual places (only when this machine has none)
        Scratch s("refuse_nokeys");
        const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
        if (find_key_files().empty()) {
            SetupOptions o = base_opts(d.path, d.keys_file_path, d.build_id, s.path("out"));
            o.key_files.clear();
            expect_refusal(o, Fail::Keys, "No Switch keys file");
        } else {
            std::printf("  skipped: keys were found in the usual places on this machine\n");
        }
    }
    {   // the input does not exist
        Scratch s("refuse_missing");
        const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
        expect_refusal(base_opts(s.path("nope.nsp"), d.keys_file_path, d.build_id, s.path("out")), Fail::Input, "");
    }
    {   // not a dump at all
        Scratch s("refuse_garbage");
        const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
        write_bytes(s.path("garbage.nsp"), pattern(4096, 7));
        expect_refusal(base_opts(s.path("garbage.nsp"), d.keys_file_path, d.build_id, s.path("out")), Fail::Input, "");
    }
}

// ---- case 6: inspect_input reports free and needed space -------------------------------------------------------------------
void case_inspect_disk() {
    Scratch s("disk");
    const SynthFiles files = romfs_files();
    const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
    SetupOptions o = base_opts(d.path, d.keys_file_path, d.build_id, s.path("out_not_created_yet"));
    const InputSummary in = inspect_input(o);
    CHECK(in.supported);
    CHECK(in.disk_free > 0);
    CHECK(in.disk_needed > 0);
    CHECK(in.disk_needed >= total_bytes(files));
}

// ---- case 7: shader cache install -----------------------------------------------------------------------------------------
void case_shader_cache() {
    Scratch s("shader");
    const SynthFiles files = romfs_files();
    const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
    const std::string src = make_shader_folder(s.path("shaders"));

    const std::string out = s.path("out");
    SetupOptions o = base_opts(d.path, d.keys_file_path, d.build_id, out);
    o.shader_cache_src = src;
    RecordingReporter rec;
    const RunResult rr = run(o, rec);
    CHECK(!rr.out.threw);
    CHECK(distinct_runs(rec.begins) == kOrderNoTextures);

    const fs::path spirv = fs::path(out) / "shader_cache" / "spirv";
    CHECK(same_bytes((spirv / "results.tsv").string(), read_bytes((fs::path(src) / "results.tsv").string())));
    for (const char* spv : {"0123456789abcdef.spv", "00004b0d6e7742b4.spv", "fedcba9876543210.spv"})
        CHECK(same_bytes((spirv / spv).string(), read_bytes((fs::path(src) / spv).string())));
    CHECK_EQ(count_files(spirv.string()), size_t(4));   // results.tsv + 3 .spv (the manifest is not installed)
    CHECK(has(as_text(read_bytes((fs::path(out) / "mpr.ini").string())), "shader_cache = shader_cache/spirv"));

    // Without a shader cache: no line in mpr.ini, no shader folder.
    const std::string out2 = s.path("out_noshaders");
    SetupOptions plain = base_opts(d.path, d.keys_file_path, d.build_id, out2);
    const RunResult rr2 = run(plain, *std::make_unique<RecordingReporter>());
    CHECK(!rr2.out.threw);
    CHECK(!has(as_text(read_bytes((fs::path(out2) / "mpr.ini").string())), "shader_cache ="));
    CHECK(!fs::exists(fs::path(out2) / "shader_cache"));
}

// ---- case 7b: a shader pack file given with shader_cache_src ----------------------------------------------------------------
void case_shader_pack_file() {
    Scratch s("shaderpack");
    const SynthFiles files = romfs_files();
    const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
    const std::string pack = s.path("shader_cache.mprshc");
    write_bytes(pack, make_shader_pack_bytes());

    const std::string out = s.path("out");
    SetupOptions o = base_opts(d.path, d.keys_file_path, d.build_id, out);
    o.allow_no_shaders = false;
    o.shader_cache_src = pack;
    RecordingReporter rec;
    const RunResult rr = run(o, rec);
    CHECK(!rr.out.threw);
    CHECK(distinct_runs(rec.begins) == kOrderNoTextures);

    const fs::path spirv = fs::path(out) / "shader_cache" / "spirv";
    for (const auto& item : shader_files()) CHECK(same_bytes((spirv / item.first).string(), item.second));
    CHECK_EQ(count_files(spirv.string()), size_t(4));   // results.tsv + 3 .spv
    CHECK(has(as_text(read_bytes((fs::path(out) / "mpr.ini").string())), "shader_cache = shader_cache/spirv"));
}

// ---- case 7c: no shader pack and no --no-shaders: setup stops before the extraction, with the message -----------------------
void case_no_shader_pack() {
    Scratch s("nopack");
    const SynthFiles files = romfs_files();
    const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
    SetupOptions o = base_opts(d.path, d.keys_file_path, d.build_id, s.path("out"));
    o.allow_no_shaders = false;
    expect_refusal(o, Fail::Input, "shader_cache.mprshc");
}

// ---- case 7d: --no-shaders installs without shaders and says so --------------------------------------------------------------
void case_no_shaders_flag() {
    Scratch s("noflag");
    const SynthFiles files = romfs_files();
    const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
    const std::string out = s.path("out");
    SetupOptions o = base_opts(d.path, d.keys_file_path, d.build_id, out);
    CHECK(o.allow_no_shaders);
    RecordingReporter rec;
    const RunResult rr = run(o, rec);
    CHECK(!rr.out.threw);
    CHECK(distinct_runs(rec.begins) == kOrderNoTextures);
    CHECK(!has(as_text(read_bytes((fs::path(out) / "mpr.ini").string())), "shader_cache ="));
    CHECK(!fs::exists(fs::path(out) / "shader_cache"));
}

// ---- case 7e: an earlier complete install is kept when no pack is given ------------------------------------------------------
void case_existing_shader_install() {
    Scratch s("keep");
    const SynthFiles files = romfs_files();
    const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
    const std::string out = s.path("out");

    SetupOptions first = base_opts(d.path, d.keys_file_path, d.build_id, out);
    first.allow_no_shaders = false;
    first.shader_cache_src = make_shader_folder(s.path("shaders"));
    CHECK(!run(first, *std::make_unique<RecordingReporter>()).out.threw);

    // Second run: no pack at all, and the shaders from the first run must be kept (and stay in mpr.ini).
    SetupOptions again = base_opts(d.path, d.keys_file_path, d.build_id, out);
    again.allow_no_shaders = false;
    RecordingReporter rec;
    const RunResult rr = run(again, rec);
    CHECK(!rr.out.threw);
    CHECK(distinct_runs(rec.begins) == kOrderNoTextures);
    const fs::path spirv = fs::path(out) / "shader_cache" / "spirv";
    CHECK_EQ(count_files(spirv.string()), size_t(4));
    CHECK(has(as_text(read_bytes((fs::path(out) / "mpr.ini").string())), "shader_cache = shader_cache/spirv"));
}

// ---- case 8: a hand-edited mpr.ini is left alone ----------------------------------------------------------------------------
void case_ini_protection() {
    Scratch s("ini");
    const SynthFiles files = romfs_files();
    const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
    const std::string out = s.path("out");
    SetupOptions o = base_opts(d.path, d.keys_file_path, d.build_id, out);
    CHECK(!run(o, *std::make_unique<RecordingReporter>()).out.threw);

    const std::string ini = (fs::path(out) / "mpr.ini").string();
    const std::vector<uint8_t> hand = to_bytes("[nvn]\nbc7_quality = 5\n# my own edits\n");
    write_bytes(ini, hand);

    o.bc7_quality = 6;
    const RunResult rr = run(o, *std::make_unique<RecordingReporter>());
    CHECK(!rr.out.threw);
    CHECK(read_bytes(ini) == hand);
}

// ---- case 9: one thread and eight threads give the same folder -------------------------------------------------------------
void case_threads() {
    Scratch s("threads");
    const SynthFiles files = romfs_files();
    const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
    const std::string one = s.path("out_t1"), eight = s.path("out_t8");

    SetupOptions a = base_opts(d.path, d.keys_file_path, d.build_id, one);
    a.threads = 1;
    CHECK(!run(a, *std::make_unique<RecordingReporter>()).out.threw);
    SetupOptions b = base_opts(d.path, d.keys_file_path, d.build_id, eight);
    b.threads = 8;
    CHECK(!run(b, *std::make_unique<RecordingReporter>()).out.threw);

    CHECK(trees_equal(one, eight));
    check_installed(eight, d.build_id, files);
}

// ---- extra: textures stage on a RomFS without .pak files -------------------------------------------------------------------
void case_textures_without_paks() {
    Scratch s("tex_nopak");
    const SynthFiles files = romfs_files();
    const SynthDump d = mprs_test::make_synth_dump("nsp", kProgramId, kBuildId, files, false, s.path("dump"));
    const std::string out = s.path("out");
    SetupOptions o = base_opts(d.path, d.keys_file_path, d.build_id, out);
    o.skip_textures = false;

    RecordingReporter rec;
    const RunResult rr = run(o, rec);
    CHECK(!rr.out.threw);
    CHECK(distinct_runs(rec.begins) == kOrderWithTextures);
    CHECK_EQ(rr.rep.textures_total, u32(0));
    CHECK_EQ(rr.rep.textures_converted, u32(0));
    CHECK_EQ(rr.rep.textures_failed, u32(0));
    check_installed(out, d.build_id, files);
}

}  // namespace

int main() {
    case_install("nsp_plain", "nsp", false);
    case_install("nsp_ticket", "nsp", true);
    case_install("xci", "xci", false);
    case_idempotent();
    case_resume();
    case_refusals();
    case_inspect_disk();
    case_shader_cache();
    case_shader_pack_file();
    case_no_shader_pack();
    case_no_shaders_flag();
    case_existing_shader_install();
    case_ini_protection();
    case_threads();
    case_textures_without_paks();
    return TEST_SUMMARY("test_pipeline");
}
