// SPDX-License-Identifier: GPL-3.0-or-later
// FAKE pipeline for the GUI test build (CMake option MPRS_GUI_SIM). It reads and writes nothing; it sleeps and reports
// believable numbers so the window can be driven and checked. Never part of the shipped tool.
//   input file name contains "bad"  -> inspect says the dump is not supported
//   input file name contains "boom" -> run_setup fails midway with Fail::Input
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "keys.h"
#include "log.h"
#include "pipeline.h"

namespace mprs {
namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr u64 kExefsBytes = 96'000'000;
constexpr u64 kRomfsBytes = 7'200'000'000;
constexpr u64 kShaderBytes = 40'000'000;
constexpr u32 kTextures = 4369;
constexpr u64 kNeededBytes = 13'000'000'000;

// True when the file name (not the folder) contains the word, case-insensitively.
bool file_name_has(const std::string& path, const wchar_t* word) {
    std::wstring name = fs::path(widen(path)).filename().wstring();
    for (auto& c : name) c = static_cast<wchar_t>(std::towlower(c));
    return name.find(word) != std::wstring::npos;
}

// Free bytes on the drive of `dir`; 0 if unknown.
u64 free_bytes_for(const std::string& dir) {
    std::wstring w = widen(dir);
    std::wstring root = (w.size() >= 3 && w[1] == L':') ? w.substr(0, 3) : std::wstring(L".");
    ULARGE_INTEGER avail{}, total{}, freeb{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &freeb)) return 0;
    return static_cast<u64>(avail.QuadPart);
}

std::string container_for(const std::string& path) {
    std::wstring ext = fs::path(widen(path)).extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    if (ext == L".xci") return "XCI";
    if (ext == L".nsz") return "NSZ";
    if (ext == L".xcz") return "XCZ";
    return "NSP";
}

void check_cancel(Reporter& r) {
    if (r.cancel_requested()) throw Error(Fail::Cancelled, "Stopped. The progress so far is kept; run setup again to continue.");
}

// Fake stage: `steps` steps spread evenly over `duration`, reporting done/total. Steps are paced against absolute target
// times, so Windows' ~16 ms timer slices do not stretch the stage. Throws Input midway when `boom` is set.
double run_steps(Reporter& r, Stage st, const char* title, u64 total, const char* unit, int steps,
                 std::chrono::milliseconds duration, bool boom) {
    r.stage_begin(st, title, total, unit);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= steps; ++i) {
        check_cancel(r);
        std::this_thread::sleep_until(t0 + duration * i / steps);
        r.stage_progress(total * static_cast<u64>(i) / static_cast<u64>(steps));
        if (boom && i == steps / 2)
            throw Error(Fail::Input, "The dump is damaged: the game data is cut short halfway through. Dump the game again and choose the new file.");
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.stage_end(st, secs);
    return secs;
}

}  // namespace

const char* stage_name(Stage s) {
    switch (s) {
        case Stage::Inspect: return "Inspect";
        case Stage::Space: return "Space";
        case Stage::Exefs: return "Exefs";
        case Stage::Romfs: return "Romfs";
        case Stage::Textures: return "Textures";
        case Stage::ShaderCache: return "ShaderCache";
        case Stage::Finish: return "Finish";
        default: return "?";
    }
}

std::vector<KeyFileCandidate> find_key_files() {
    std::vector<KeyFileCandidate> out;
    if (const char* sim = std::getenv("MPRS_SIM_KEYS"); sim && *sim) {
        if (fs::exists(widen(sim))) out.push_back({sim, "simulated"});
        return out;
    }
    struct Place { const wchar_t* env; const wchar_t* sub; const char* source; };
    static const Place kPlaces[] = {
        {L"APPDATA", L"yuzu\\keys", "yuzu"},
        {L"APPDATA", L"eden\\keys", "Eden"},
        {L"APPDATA", L"suyu\\keys", "suyu"},
        {L"APPDATA", L"sudachi\\keys", "Sudachi"},
        {L"APPDATA", L"citron\\keys", "Citron"},
        {L"APPDATA", L"Ryujinx\\system", "Ryujinx"},
        {L"USERPROFILE", L".switch", "~/.switch"},
    };
    for (const Place& p : kPlaces) {
        const wchar_t* base = _wgetenv(p.env);
        if (!base) continue;
        fs::path f = fs::path(base) / p.sub / L"prod.keys";
        std::error_code ec;
        if (fs::exists(f, ec)) out.push_back({narrow(f.wstring()), p.source});
    }
    return out;
}

std::string sibling_title_keys(const std::string& prod_keys_path) {
    fs::path t = fs::path(widen(prod_keys_path)).parent_path() / L"title.keys";
    std::error_code ec;
    return fs::exists(t, ec) ? narrow(t.wstring()) : std::string();
}

InputSummary inspect_input(const SetupOptions& o) {
    std::this_thread::sleep_for(1s);
    InputSummary s;
    s.container = container_for(o.input_path);
    s.title_id = "010012101468C000";
    s.build_id = "67C9CA1428222752F951021464A7B3597EB98FEC";
    s.romfs_bytes = kRomfsBytes;
    s.exefs_bytes = kExefsBytes;
    s.romfs_files = 2817;
    s.astc_textures = kTextures;
    s.bc7_bytes_estimate = 2'900'000'000;
    s.disk_needed = kNeededBytes;
    s.disk_free = free_bytes_for(o.out_dir);

    if (file_name_has(o.input_path, L"bad")) {
        s.supported = false;
        s.problem = "This dump is the wrong game (title 0100ABCDEF000000). Choose the Metroid Prime Remastered dump.";
    } else if (s.disk_free && s.disk_free < s.disk_needed) {
        s.supported = false;
        s.problem = "Not enough free space on this drive for the game folder. Choose another install folder.";
    } else {
        s.supported = true;
    }
    return s;
}

SetupReport run_setup(const SetupOptions& o, Reporter& r) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool boom = file_name_has(o.input_path, L"boom");
    MPRS_LOGI("Simulated setup (fake pipeline, nothing is written) for %s", o.input_path.c_str());

    SetupReport rep;
    rep.seconds[static_cast<int>(Stage::Exefs)] =
        run_steps(r, Stage::Exefs, "Copying the program files", kExefsBytes, "bytes", 20, 1000ms, false);
    MPRS_LOGI("Program files copied");

    rep.seconds[static_cast<int>(Stage::Romfs)] = run_steps(r, Stage::Romfs, "Extracting the game files (RomFS)", kRomfsBytes,
                                                             "bytes", 200, 20000ms, boom);
    MPRS_LOGI("Game files extracted");

    rep.seconds[static_cast<int>(Stage::Textures)] =
        run_steps(r, Stage::Textures, "Converting textures for fast loading", kTextures, "textures", kTextures, 30000ms, false);
    MPRS_LOGI("Textures converted");

    rep.seconds[static_cast<int>(Stage::ShaderCache)] =
        run_steps(r, Stage::ShaderCache, "Preparing the shader cache", kShaderBytes, "bytes", 30, 3000ms, false);

    r.stage_begin(Stage::Finish, "Finishing up", 0, "bytes");
    const auto tf = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(1s);
    check_cancel(r);
    rep.seconds[static_cast<int>(Stage::Finish)] = std::chrono::duration<double>(std::chrono::steady_clock::now() - tf).count();
    r.stage_end(Stage::Finish, rep.seconds[static_cast<int>(Stage::Finish)]);

    rep.files_written = 9412;
    rep.files_skipped = 0;
    rep.bytes_written = kExefsBytes + kRomfsBytes + 2'900'000'000ull;
    rep.textures_total = kTextures;
    rep.textures_converted = kTextures;
    rep.textures_skipped = 0;
    rep.textures_failed = 0;
    rep.total_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    MPRS_LOGI("Simulated setup finished");
    return rep;
}

}  // namespace mprs
