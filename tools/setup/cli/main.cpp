// SPDX-License-Identifier: GPL-3.0-or-later
// mpr_setup_cli: the setup as a command line tool (automation, headless machines, support).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "pipeline.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace mprs;

namespace {

std::atomic<bool> g_cancel{false};
BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_cancel = true;
        return TRUE;
    }
    return FALSE;
}

void usage() {
    std::puts(
        "Usage: mpr_setup_cli --input <dump.nsp|xci|nsz|xcz> --out <game folder> [options]\n"
        "\n"
        "  --keys <prod.keys>[,<title.keys>]   keys file(s); default: look in the usual emulator folders\n"
        "  --quality <0-6>                     texture conversion quality (default 3; the game reads the same value from mpr.ini)\n"
        "  --threads <n>                       worker threads (default: all)\n"
        "  --shader-cache <folder|pack>        install this shader cache (default: shader_cache next to this program, if any)\n"
        "  --skip-textures                     do not build the texture cache\n"
        "  --no-verify                         skip checking the dump's own hashes (faster, less safe)\n"
        "  --verify-output                     re-hash files that already exist instead of trusting their size\n"
        "  --check                             open and validate the dump, print what it is, write nothing\n"
        "  --find-keys                         list key files found in the usual places and exit\n"
        "  --title-id <16 hex> --build-id <40 hex>   (testing) accept another game\n"
        "\n"
        "Exit codes: 0 done, 1 error, 2 bad dump, 3 keys, 4 disk, 5 stopped (Ctrl+C; run again to continue).");
}

class ConsoleReporter : public Reporter {
public:
    void stage_begin(Stage s, const std::string& title, u64 total, const char* unit) override {
        std::lock_guard lk(mu_);
        newline();
        stage_ = title;
        total_ = total;
        unit_ = unit ? unit : "";
        done_ = 0;
        t0_ = std::chrono::steady_clock::now();
        last_print_ = {};
        (void)s;
    }
    void stage_progress(u64 done) override {
        std::lock_guard lk(mu_);
        done_ = done;
        auto now = std::chrono::steady_clock::now();
        if (now - last_print_ < std::chrono::milliseconds(500)) return;
        last_print_ = now;
        print_line();
    }
    void stage_end(Stage, double) override {
        std::lock_guard lk(mu_);
        if (inline_) { done_ = total_; print_line(); }
        newline();
    }
    bool cancel_requested() override { return g_cancel.load(); }
    void log(const std::string& s) {
        std::lock_guard lk(mu_);
        newline();
        std::printf("%s\n", s.c_str());
        std::fflush(stdout);
    }
private:
    void print_line() {
        char buf[200];
        if (total_ == 0) {
            std::snprintf(buf, sizeof buf, "  %s ...", stage_.c_str());
        } else if (unit_ == "bytes") {
            std::snprintf(buf, sizeof buf, "  %s: %.2f / %.2f GB (%.0f%%)", stage_.c_str(), done_ / 1e9, total_ / 1e9, 100.0 * done_ / total_);
        } else {
            std::snprintf(buf, sizeof buf, "  %s: %llu / %llu %s (%.0f%%)", stage_.c_str(), (unsigned long long)done_,
                          (unsigned long long)total_, unit_.c_str(), 100.0 * done_ / total_);
        }
        std::printf("\r%-79s", buf);
        std::fflush(stdout);
        inline_ = true;
    }
    void newline() {
        if (inline_) { std::printf("\n"); inline_ = false; }
    }
    std::mutex mu_;
    std::string stage_, unit_;
    u64 total_ = 0, done_ = 0;
    bool inline_ = false;
    std::chrono::steady_clock::time_point t0_, last_print_;
};

std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> v;
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find(',', p);
        if (q == std::string::npos) q = s.size();
        if (q > p) v.push_back(s.substr(p, q - p));
        p = q + 1;
    }
    return v;
}

}  // namespace

int wmain(int argc, wchar_t** wargv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(narrow(wargv[i]));
    SetupOptions o;
    bool check = false, find_keys = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= args.size()) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(1); }
            return args[++i];
        };
        if (a == "--input") o.input_path = val();
        else if (a == "--out") o.out_dir = val();
        else if (a == "--keys") o.key_files = split_commas(val());
        else if (a == "--quality") o.bc7_quality = std::atoi(val().c_str());
        else if (a == "--threads") o.threads = std::atoi(val().c_str());
        else if (a == "--shader-cache") o.shader_cache_src = val();
        else if (a == "--skip-textures") o.skip_textures = true;
        else if (a == "--no-verify") o.verify_input = false;
        else if (a == "--verify-output") o.verify_output = true;
        else if (a == "--check") check = true;
        else if (a == "--find-keys") find_keys = true;
        else if (a == "--title-id") o.expected_title_id = val();
        else if (a == "--build-id") o.expected_build_id = val();
        else if (a == "--help" || a == "-h" || a == "/?") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n\n", a.c_str()); usage(); return 1; }
    }
    if (find_keys) {
        auto k = find_key_files();
        for (auto& c : k) std::printf("%s  (%s)\n", c.path.c_str(), c.source.c_str());
        if (k.empty()) std::puts("no key files found");
        return 0;
    }
    if (o.input_path.empty() || (o.out_dir.empty() && !check)) { usage(); return 1; }
    if (o.bc7_quality < 0 || o.bc7_quality > 6) { std::fprintf(stderr, "--quality must be 0..6\n"); return 1; }
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    ConsoleReporter rep;
    log_set_sink([&](LogLevel lv, const std::string& m) { rep.log((lv == LogLevel::Warn ? "warning: " : lv == LogLevel::Error ? "error: " : "") + m); },
                 LogLevel::Info);
    try {
        if (check) {
            InputSummary s = inspect_input(o);
            std::printf("container: %s\ntitle id: %s\nversion: %s\nbuild id: %s\nRomFS: %u files, %.2f GB\n", s.container.c_str(),
                        s.title_id.c_str(), s.version.c_str(), s.build_id.c_str(), s.romfs_files, s.romfs_bytes / 1e9);
            if (!o.out_dir.empty())
                std::printf("disk: needs %.1f GB, %.1f GB free\n", s.disk_needed / 1e9, s.disk_free / 1e9);
            if (!s.supported) { std::printf("NOT SUPPORTED: %s\n", s.problem.c_str()); return 2; }
            std::puts("supported");
            return 0;
        }
        SetupReport r = run_setup(o, rep);
        std::printf("\nDone in %.0f s. %llu files written, %llu already present; textures: %u converted, %u already done.\n",
                    r.total_seconds, (unsigned long long)r.files_written, (unsigned long long)r.files_skipped, r.textures_converted,
                    r.textures_skipped);
        return 0;
    } catch (const Error& e) {
        std::fprintf(stderr, "\n%s\n", e.what());
        return static_cast<int>(e.kind);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "\nUnexpected error: %s\n", e.what());
        return 1;
    }
}
