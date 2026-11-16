// SPDX-License-Identifier: GPL-3.0-or-later
// The setup window: SDL3 + Dear ImGui (base/ui, nxui), the same program on Windows and Linux. The player picks the game dump and the Switch
// keys, picks the install folder and presses Start. The pipeline runs on a worker thread and reports only through the mailbox below; every
// control is touched on the window thread. Paths and text are UTF-8 throughout.
//   <id>_setup [--input <file>]... [--keys <file>[,<file>]] [--out <dir>] [--quality N] [--auto-start] [--exit-when-done]
//              [--no-message-box] [--shader-pack <file or folder>] [--no-shaders]
// --exit-when-done closes the window when the run ends (exit code 0 on success, else the Fail kind; 10 if the dump is not
// supported) and skips the success summary box. --no-message-box suppresses every message box.

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "game.h"
#include "keys.h"
#include "log.h"
#include "nxui/ui.h"
#include "pipeline.h"

namespace {

using nxs::Fail;
using nxs::LogLevel;
using nxs::Stage;
using nxs::u64;

#ifndef NXS_VERSION
#define NXS_VERSION "dev"
#endif

constexpr int kMaxLogLines = 2000;
constexpr int kLogTrimLines = 500;
constexpr int kLogicalW = 644;  // client area in logical px (96 DPI)
constexpr int kLogicalH = 580;
constexpr const char* kPathSep = " | ";
constexpr const char* kDumpHint =
    "Pick your own game dump (.nsp, .xci, .nsz or .xcz), or drag it onto this window. Pick the update and DLC files with it if you have them.";
constexpr const char* kKeysHint =
    "Browse to your prod.keys (dumped from your own Switch with Lockpick_RCM). Nothing is uploaded; the file is only read.";
constexpr const char* kIdleTitle = "Choose your game dump and keys, then click Start setup.";
constexpr const char* kStopNote = "Stopping after the current step. Progress is kept; run setup again to continue.";
constexpr const char* kStoppedTitle = "Stopped. Progress is kept; run setup again to continue.";

std::filesystem::path fs_path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string trim_path(std::string s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.front())) s.erase(s.begin());
    while (!s.empty() && ws(s.back())) s.pop_back();
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    while (!s.empty() && ws(s.front())) s.erase(s.begin());
    while (!s.empty() && ws(s.back())) s.pop_back();
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extension_lower(const std::string& path) {
    const size_t sep = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return {};
    return lower(path.substr(dot));
}

std::string absolute_path(const std::string& p) {
    std::error_code ec;
    const auto a = std::filesystem::absolute(fs_path(p), ec);
    if (ec) return p;
    const auto u = a.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// The program's own folder, UTF-8, without a trailing separator.
std::string exe_folder() {
    const char* b = SDL_GetBasePath();
    std::string s = b ? b : ".";
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

// The dump field holds one or more paths joined by " | " ('|' cannot be part of a path).
std::vector<std::string> split_dump_paths(const std::string& s) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find('|', p);
        if (q == std::string::npos) q = s.size();
        std::string one = trim_path(s.substr(p, q - p));
        if (!one.empty()) out.push_back(one);
        p = q + 1;
    }
    return out;
}
std::string join_dump_paths(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) {
        if (!s.empty()) s += kPathSep;
        s += x;
    }
    return s;
}

std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

struct CmdLine {
    std::string input;
    std::string out;
    std::vector<std::string> keys;
    int quality = -1;
    bool auto_start = false;
    bool exit_when_done = false;
    bool no_message_box = false;
    std::string shader_pack;   // --shader-pack <file or folder>; empty = look next to this program
    bool no_shaders = false;   // --no-shaders (developer runs)
};

CmdLine parse_command_line(int argc, char** argv) {
    CmdLine c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--input") {
            const std::string v = trim_path(value());
            if (!v.empty()) c.input += (c.input.empty() ? "" : kPathSep) + v;
        } else if (a == "--out") {
            c.out = trim_path(value());
        } else if (a == "--keys") {
            for (auto& k : split_commas(value())) c.keys.push_back(trim_path(k));
        } else if (a == "--quality") {
            const int q = std::atoi(value().c_str());
            if (q >= 0 && q <= 6) c.quality = q;
        } else if (a == "--auto-start") {
            c.auto_start = true;
        } else if (a == "--exit-when-done") {
            c.exit_when_done = true;
        } else if (a == "--no-message-box") {
            c.no_message_box = true;
        } else if (a == "--shader-pack") {
            c.shader_pack = trim_path(value());
        } else if (a == "--no-shaders") {
            c.no_shaders = true;
        }
    }
    {
        std::vector<std::string> in = split_dump_paths(c.input);
        for (auto& x : in) x = absolute_path(x);
        c.input = join_dump_paths(in);
    }
    for (auto* p : {&c.out, &c.shader_pack})
        if (!p->empty()) *p = absolute_path(*p);
    for (auto& k : c.keys) k = absolute_path(k);
    return c;
}

std::string size_text(u64 bytes) {
    const double b = static_cast<double>(bytes);
    if (b >= 1e9) return std::format("{:.1f} GB", b / 1e9);
    return std::format("{:.0f} MB", b / 1e6);
}

std::string duration_text(double secs) {
    const long long total = std::llround(secs);
    const long long m = total / 60, s = total % 60;
    return m > 0 ? std::format("{} min {} s", m, s) : std::format("{} s", s);
}

std::string eta_text(double secs) {
    if (secs < 90) return std::format("about {:.0f} s left", secs);
    return std::format("about {:.0f} min left", secs / 60);
}

std::string clock_text(u64 ms) {
    const u64 s = ms / 1000;
    if (s >= 3600) return std::format("{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60);
    return std::format("{:02}:{:02}", s / 60, s % 60);
}

double stage_weight(Stage s) {
    switch (s) {
        case Stage::Exefs: return 1;
        case Stage::Romfs: return 25;
        case Stage::Textures: return 70;
        case Stage::ShaderCache: return 3;
        case Stage::Finish: return 1;
        default: return 0;
    }
}

const char* title_for(Fail k) {
    switch (k) {
        case Fail::Input: return "This dump can't be used";
        case Fail::Keys: return "Keys problem";
        case Fail::Disk: return "Disk problem";
        case Fail::Cancelled: return "Stopped";
        default: return "Setup failed";
    }
}

struct InspectMsg {
    std::string key;
    nxs::InputSummary summary;
};
struct RunMsg {
    bool ok = false;
    bool unexpected = false;
    Fail kind = Fail::Generic;
    std::string text;
    nxs::SetupReport report;
};

// Progress values the worker writes and the window reads. Lives in the App, which outlives the worker thread.
struct RunShared {
    std::atomic<bool> cancel{false};
    std::atomic<u64> done{0};
};

// What the worker threads hand to the window thread: functions run on the next frame.
class Mailbox {
public:
    void post(std::function<void()> f) {
        std::lock_guard<std::mutex> lk(mu_);
        q_.push_back(std::move(f));
    }
    void drain() {
        std::vector<std::function<void()>> v;
        {
            std::lock_guard<std::mutex> lk(mu_);
            v.swap(q_);
        }
        for (auto& f : v) f();
    }

private:
    std::mutex mu_;
    std::vector<std::function<void()>> q_;
};

// Worker thread: opens the dump and reports the result. Never throws out of the thread.
void inspect_worker(Mailbox* box, nxui::App* ui, std::function<void(std::unique_ptr<InspectMsg>)> deliver, std::string key, nxs::SetupOptions o) {
    std::shared_ptr<InspectMsg> msg;
    try {
        msg = std::make_shared<InspectMsg>();
        msg->key = std::move(key);
        try {
            msg->summary = nxs::inspect_input(o);
        } catch (const nxs::Error& e) {
            msg->summary.supported = false;
            msg->summary.problem = e.what();
        } catch (const std::exception& e) {
            msg->summary.supported = false;
            msg->summary.problem = std::string("Unexpected error: ") + e.what();
        } catch (...) {
            msg->summary.supported = false;
            msg->summary.problem = "Unexpected error: unknown exception.";
        }
    } catch (...) {
        return;
    }
    box->post([msg, deliver]() mutable { deliver(std::unique_ptr<InspectMsg>(new InspectMsg(std::move(*msg)))); });
    ui->wake();
}

// Reports pipeline progress to the window thread.
class GuiReporter final : public nxs::Reporter {
public:
    GuiReporter(Mailbox* box, nxui::App* ui, RunShared& shared, std::function<void(Stage, std::string, u64, bool, bool)> begin,
                std::function<void(Stage, double)> end)
        : box_(box), ui_(ui), shared_(shared), begin_(std::move(begin)), end_(std::move(end)) {}

    void stage_begin(Stage stage, const std::string& title, u64 total, const char* unit) override {
        shared_.done.store(0);
        const bool textures = unit && std::strcmp(unit, "textures") == 0;
        const bool percent = unit && std::strcmp(unit, "megapixels") == 0;
        const bool bytes = !textures && !percent;
        box_->post([this, stage, title, total, bytes, percent] { begin_(stage, title, total, bytes, percent); });
        ui_->wake();
    }

    void stage_progress(u64 done) override { shared_.done.store(done, std::memory_order_relaxed); }   // read by the window every frame

    void stage_end(Stage stage, double seconds) override {
        box_->post([this, stage, seconds] { end_(stage, seconds); });
        ui_->wake();
    }

    bool cancel_requested() override { return shared_.cancel.load(); }

private:
    Mailbox* box_;
    nxui::App* ui_;
    RunShared& shared_;
    std::function<void(Stage, std::string, u64, bool, bool)> begin_;
    std::function<void(Stage, double)> end_;
};

class App {
public:
    explicit App(CmdLine cli) : cli_(std::move(cli)) {}
    int run();

private:
    // ---- state helpers
    std::string out_text() const { return trim_path(out_text_); }
    std::vector<std::string> keys_for_run() const;
    std::string current_key() const;
    nxs::SetupOptions make_options() const;
    int quality_value() const;
    bool can_start() const { return !running_ && supported_ && !dump_path_.empty() && !keys_text_.empty() && applied_key_ == key_cache_; }

    // ---- actions
    void browse_dump();
    void browse_keys();
    void browse_folder();
    void set_dump(const std::string& path);
    void add_dumps(const std::vector<std::string>& paths);
    void set_keys(const std::string& path, const std::string& source);
    void update_keys_hint();
    void keys_or_out_edited();
    void request_inspect();
    void start_inspect(const std::string& key);
    void on_inspected(std::unique_ptr<InspectMsg> msg);
    void show_inspect_result();
    void start_run();
    void request_stop();
    void on_run_done(std::unique_ptr<RunMsg> r);
    void on_stage_begin(Stage stage, const std::string& title, u64 total, bool bytes, bool percent);
    void on_stage_end(Stage stage, double seconds);
    void append_log(const std::string& line);
    void refresh_progress();
    void show_box(const std::string& title, const std::string& text, const ImVec4& color = ImVec4(0, 0, 0, 0));
    bool on_event(const SDL_Event& ev);
    void frame();
    void draw_path_row(float& y, const char* label, const char* id, std::string* text, bool editable, float extra_btn_w, const char* extra_label,
                       const std::function<void()>& extra, const std::function<void()>& browse);

    CmdLine cli_;
    nxui::App ui_;
    Mailbox mail_;

    std::string dump_path_;
    std::string keys_text_;
    std::string out_text_;
    std::string prev_keys_, prev_out_;
    std::vector<std::string> extra_keys_;   // keys given on the command line after the first
    std::string auto_keys_source_;          // set when the keys were found automatically
    std::vector<int> quality_values_;
    std::vector<std::string> quality_items_;
    int quality_sel_ = 0;
    std::string key_cache_;                 // current_key(), kept up to date when the inputs change

    std::string dump_status_ = kDumpHint;
    ImVec4 dump_status_color_ = nxui::col::gray;
    std::string disk_line_;
    std::string keys_hint_ = kKeysHint;
    bool auto_start_pending_ = false;
    uint64_t debounce_at_ms_ = 0;           // 0: no check scheduled

    std::unique_ptr<std::thread> inspect_thread_;
    bool inspect_running_ = false;
    bool inspect_again_ = false;
    std::string applied_key_;               // inputs the current result belongs to
    nxs::InputSummary summary_;
    bool supported_ = false;

    std::unique_ptr<std::thread> run_thread_;
    RunShared shared_;
    bool running_ = false;
    bool stop_requested_ = false;
    bool close_after_run_ = false;
    bool run_success_ = false;
    Stage stage_ = Stage::Inspect;
    bool stage_active_ = false;
    u64 stage_total_ = 0;
    bool stage_bytes_ = true;
    bool stage_percent_ = false;
    u64 stage_started_ms_ = 0;
    u64 run_started_ms_ = 0;
    double completed_weight_ = 0;
    std::string stage_title_ = kIdleTitle, stage_detail_, overall_text_;
    float stage_frac_ = 0, overall_frac_ = 0;
    std::vector<std::string> log_;
    bool log_dirty_ = false;

    int exit_code_ = 0;
};

std::vector<std::string> App::keys_for_run() const {
    std::vector<std::string> v;
    const std::string first = trim_path(keys_text_);
    if (first.empty()) return v;
    v.push_back(first);
    for (const auto& e : extra_keys_) v.push_back(e);
    if (extra_keys_.empty()) {
        const std::string title = nxs::sibling_title_keys(first);
        if (!title.empty()) v.push_back(title);
    }
    return v;
}

std::string App::current_key() const {
    std::string k = dump_path_ + "\n";
    for (const auto& p : keys_for_run()) k += p + "\n";
    k += out_text();
    return k;
}

nxs::SetupOptions App::make_options() const {
    nxs::SetupOptions o;
    for (const auto& p : split_dump_paths(dump_path_)) o.input_paths.push_back(p);
    for (const auto& k : keys_for_run()) o.key_files.push_back(k);
    o.out_dir = out_text();
    o.bc7_quality = quality_value();
    o.shader_cache_src = cli_.shader_pack;
    o.allow_no_shaders = cli_.no_shaders;
    return o;
}

int App::quality_value() const {
    if (quality_sel_ < 0 || quality_sel_ >= static_cast<int>(quality_values_.size())) return 3;
    return quality_values_[static_cast<size_t>(quality_sel_)];
}

void App::show_box(const std::string& title, const std::string& text, const ImVec4& color) {
    if (cli_.no_message_box) return;
    nxui::MessageSpec m;
    m.title = title;
    m.text = text;
    m.color = color;
    ui_.message(std::move(m));
}

// ---- picking -----------------------------------------------------------------------------------------------------------------------------

void App::browse_dump() {
    ui_.pick_files("Choose your game dump (and its update and DLC)", {"Switch game dumps (*.nsp;*.xci;*.nsz;*.xcz)|nsp;xci;nsz;xcz", "All files (*.*)|*"}, true, "",
                   [this](std::vector<std::string> p) {
                       if (!p.empty()) add_dumps(p);
                   });
}

void App::browse_keys() {
    ui_.pick_files("Choose your prod.keys", {"Switch keys (*.keys)|keys", "All files (*.*)|*"}, false, "", [this](std::vector<std::string> p) {
        if (p.empty()) return;
        set_keys(p[0], "");
        request_inspect();
    });
}

void App::browse_folder() {
    ui_.pick_folder("Choose the folder to install the game to", out_text(), [this](std::string p) {
        if (p.empty()) return;
        out_text_ = p;
        prev_out_ = out_text_;
        request_inspect();
    });
}

void App::set_dump(const std::string& path) {
    dump_path_ = trim_path(path);
    request_inspect();
}

void App::add_dumps(const std::vector<std::string>& paths) {
    std::vector<std::string> v = split_dump_paths(dump_path_);
    for (const auto& p : paths) {
        const std::string t = trim_path(p);
        if (t.empty()) continue;
        if (std::none_of(v.begin(), v.end(), [&](const std::string& x) { return lower(x) == lower(t); })) v.push_back(t);
    }
    set_dump(join_dump_paths(v));
}

void App::set_keys(const std::string& path, const std::string& source) {
    extra_keys_.clear();
    auto_keys_source_ = source;
    keys_text_ = trim_path(path);
    prev_keys_ = keys_text_;
    update_keys_hint();
}

void App::update_keys_hint() { keys_hint_ = auto_keys_source_.empty() ? std::string(kKeysHint) : "Found automatically: " + auto_keys_source_; }

// The keys or the install folder were typed into: look at the dump again a moment after the typing stops.
void App::keys_or_out_edited() {
    if (keys_text_ != prev_keys_) {
        extra_keys_.clear();
        auto_keys_source_.clear();
        update_keys_hint();
    }
    prev_keys_ = keys_text_;
    prev_out_ = out_text_;
    debounce_at_ms_ = SDL_GetTicks() + 400;
}

// ---- inspecting the dump -----------------------------------------------------------------------------------------------------------

void App::request_inspect() {
    key_cache_ = current_key();
    if (dump_path_.empty()) {
        applied_key_.clear();
        supported_ = false;
        dump_status_ = kDumpHint;
        dump_status_color_ = nxui::col::gray;
        disk_line_.clear();
        return;
    }
    if (key_cache_ == applied_key_) return;
    if (inspect_running_) {
        inspect_again_ = true;
        supported_ = false;
        dump_status_ = "Checking the dump...";
        dump_status_color_ = nxui::col::gray;
        return;
    }
    start_inspect(key_cache_);
}

void App::start_inspect(const std::string& key) {
    inspect_running_ = true;
    inspect_again_ = false;
    supported_ = false;
    dump_status_ = "Checking the dump...";
    dump_status_color_ = nxui::col::gray;
    if (inspect_thread_ && inspect_thread_->joinable()) inspect_thread_->join();
    try {
        inspect_thread_ = std::make_unique<std::thread>(inspect_worker, &mail_, &ui_, [this](std::unique_ptr<InspectMsg> m) { on_inspected(std::move(m)); }, key, make_options());
    } catch (const std::exception& e) {
        inspect_running_ = false;
        dump_status_ = std::string("Could not start the check: ") + e.what();
        dump_status_color_ = nxui::col::red;
    }
}

void App::on_inspected(std::unique_ptr<InspectMsg> msg) {
    if (inspect_thread_ && inspect_thread_->joinable()) inspect_thread_->join();
    inspect_running_ = false;
    const bool current = msg->key == key_cache_;
    if (current) {
        applied_key_ = msg->key;
        summary_ = msg->summary;
        supported_ = summary_.supported;
        show_inspect_result();
    }
    if (inspect_again_ || !current) {
        inspect_again_ = false;
        request_inspect();
    }
    if (!current) return;
    if (cli_.exit_when_done && !running_ && !supported_) {
        exit_code_ = 10;
        ui_.quit(exit_code_);
        return;
    }
    if (auto_start_pending_ && can_start()) {
        auto_start_pending_ = false;
        start_run();
    }
}

void App::show_inspect_result() {
    if (summary_.supported) {
        std::string extra;
        if (!summary_.update.empty()) extra += ", update " + summary_.update;
        if (summary_.dlc_count) extra += std::format(", {} DLC ({})", summary_.dlc_count, size_text(summary_.dlc_bytes));
        std::string text = std::format("{} found ({}{}). Needs {}, {} free.", nxs::game().title, summary_.container, extra, size_text(summary_.disk_needed),
                                       size_text(summary_.disk_free));
        if (!summary_.notes.empty()) text += " Note: " + summary_.notes[0] + (summary_.notes.size() > 1 ? " (more in the log)" : "");
        dump_status_ = text;
        dump_status_color_ = nxui::col::green;
        disk_line_ = std::format("This folder needs about {} (free: {}).", size_text(summary_.disk_needed), size_text(summary_.disk_free));
    } else {
        std::string problem = summary_.problem;
        if (problem.empty()) problem = "This is not a dump this setup can use.";
        dump_status_ = problem;
        dump_status_color_ = nxui::col::red;
        disk_line_.clear();
    }
}

// ---- running the setup -------------------------------------------------------------------------------------------------------------

void App::start_run() {
    if (!can_start()) return;
    const std::string out = out_text();
    std::error_code ec;
    std::filesystem::create_directories(fs_path(out), ec);
    if (ec) {
        show_box(title_for(Fail::Disk), "Can't create the install folder:\n" + out + "\n" + ec.message(), nxui::col::red);
        return;
    }
    nxs::log_open(out + "/setup.log");
    nxs::SetupOptions o = make_options();
    NXS_LOGI("Setup started: input %s, folder %s, texture quality %d", dump_path_.c_str(), o.out_dir.c_str(), o.bc7_quality);

    shared_.cancel = false;
    shared_.done = 0;
    running_ = true;
    stop_requested_ = false;
    close_after_run_ = false;
    run_success_ = false;
    stage_active_ = false;
    stage_total_ = 0;
    completed_weight_ = 0;
    run_started_ms_ = SDL_GetTicks();
    stage_title_ = "Starting...";
    stage_detail_.clear();
    stage_frac_ = overall_frac_ = 0;
    overall_text_ = "Overall 0%     Elapsed 00:00";
    if (run_thread_ && run_thread_->joinable()) run_thread_->join();
    try {
        run_thread_ = std::make_unique<std::thread>([this, o = std::move(o)]() {
            try {
                auto res = std::make_shared<RunMsg>();
                try {
                    GuiReporter rep(
                        &mail_, &ui_, shared_,
                        [this](Stage s, std::string t, u64 total, bool bytes, bool percent) { on_stage_begin(s, t, total, bytes, percent); },
                        [this](Stage s, double secs) { on_stage_end(s, secs); });
                    res->report = nxs::run_setup(o, rep);
                    res->ok = true;
                } catch (const nxs::Error& e) {
                    res->kind = e.kind;
                    res->text = e.what();
                } catch (const std::exception& e) {
                    res->unexpected = true;
                    res->text = std::string("Unexpected error: ") + e.what();
                } catch (...) {
                    res->unexpected = true;
                    res->text = "Unexpected error: unknown exception.";
                }
                mail_.post([this, res] { on_run_done(std::unique_ptr<RunMsg>(new RunMsg(*res))); });
                ui_.wake();
            } catch (...) {
            }
        });
    } catch (const std::exception& e) {
        running_ = false;
        stage_title_ = std::string("Could not start the setup: ") + e.what();
    }
}

void App::request_stop() {
    if (!running_ || stop_requested_) return;
    shared_.cancel = true;
    stop_requested_ = true;
    stage_title_ = kStopNote;
}

void App::on_run_done(std::unique_ptr<RunMsg> r) {
    if (run_thread_ && run_thread_->joinable()) run_thread_->join();
    running_ = false;
    stop_requested_ = false;
    stage_active_ = false;
    const u64 elapsed_ms = SDL_GetTicks() - run_started_ms_;

    if (r->ok) {
        run_success_ = true;
        const double secs = r->report.total_seconds > 0 ? r->report.total_seconds : elapsed_ms / 1000.0;
        stage_title_ = std::format("Setup finished in {}. Your game folder is ready:", duration_text(secs));
        stage_detail_.clear();
        stage_frac_ = overall_frac_ = 1.f;
        overall_text_ = std::format("Overall 100%     Elapsed {}", clock_text(elapsed_ms));
        NXS_LOGI("Setup finished in %s", duration_text(secs).c_str());
        if (!cli_.no_message_box && !cli_.exit_when_done) {
            const auto& rep = r->report;
            show_box("Setup finished",
                     std::format("Setup finished in {}.\n\nFiles written: {}\nFiles already in place: {}\nTextures converted: {}, skipped: {}, failed: {}",
                                 duration_text(secs), rep.files_written, rep.files_skipped, rep.textures_converted, rep.textures_skipped,
                                 rep.textures_failed));
        }
        exit_code_ = 0;
    } else {
        const bool cancelled = !r->unexpected && r->kind == Fail::Cancelled;
        stage_title_ = cancelled ? kStoppedTitle : "Setup did not finish. See the log below.";
        stage_detail_.clear();
        NXS_LOGE("Setup stopped: %s", r->text.c_str());
        if (!(cli_.no_message_box || (close_after_run_ && cancelled)))
            show_box(r->unexpected ? "Setup failed" : title_for(r->kind), r->text, cancelled ? ImVec4(0, 0, 0, 0) : nxui::col::red);
        exit_code_ = r->unexpected ? static_cast<int>(Fail::Generic) : static_cast<int>(r->kind);
    }
    if (close_after_run_ || cli_.exit_when_done) ui_.quit(exit_code_);
}

void App::on_stage_begin(Stage stage, const std::string& title, u64 total, bool bytes, bool percent) {
    stage_ = stage;
    stage_active_ = true;
    stage_total_ = total;
    stage_bytes_ = bytes;
    stage_percent_ = percent;
    stage_started_ms_ = SDL_GetTicks();
    stage_title_ = title;
}

void App::on_stage_end(Stage stage, double) {
    completed_weight_ += stage_weight(stage);
    stage_active_ = false;
}

void App::append_log(const std::string& line) {
    log_.push_back(line);
    if (static_cast<int>(log_.size()) > kMaxLogLines) log_.erase(log_.begin(), log_.begin() + kLogTrimLines);
    log_dirty_ = true;
}

void App::refresh_progress() {
    if (!running_) return;
    const u64 done = shared_.done.load();
    double frac = 0;
    std::string detail;
    if (stage_active_ && stage_total_ > 0) {
        frac = std::min(1.0, static_cast<double>(done) / static_cast<double>(stage_total_));
        detail = stage_percent_ ? std::format("{}% done", static_cast<int>(frac * 100))
                 : stage_bytes_ ? std::format("{} of {}", size_text(done), size_text(stage_total_))
                                : std::format("{} of {} textures", done, stage_total_);
        const double stage_secs = (SDL_GetTicks() - stage_started_ms_) / 1000.0;
        if (done > 0 && done < stage_total_ && stage_secs >= 5) {
            const double rate = static_cast<double>(done) / stage_secs;
            const double left = static_cast<double>(stage_total_ - done) / rate;
            detail += "   -   " + eta_text(left);
        }
    }
    stage_frac_ = static_cast<float>(frac);
    stage_detail_ = detail;
    const double overall = completed_weight_ + (stage_active_ ? stage_weight(stage_) * frac : 0.0);
    overall_frac_ = static_cast<float>(std::clamp(overall / 100.0, 0.0, 1.0));
    overall_text_ = std::format("Overall {:.0f}%     Elapsed {}", overall, clock_text(SDL_GetTicks() - run_started_ms_));
}

// ---- events and frames -------------------------------------------------------------------------------------------------------------

bool App::on_event(const SDL_Event& ev) {
    if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data) {
        if (running_) return true;
        const std::string path = ev.drop.data;
        if (extension_lower(path) == ".keys") {
            set_keys(path, "");
            request_inspect();
        } else {
            add_dumps({path});
        }
        return true;
    }
    return false;
}

// One label row: a bold label, then a box and the buttons.
void App::draw_path_row(float& y, const char* label, const char* id, std::string* text, bool editable, float extra_btn_w, const char* extra_label,
                        const std::function<void()>& extra, const std::function<void()>& browse) {
    const float S = ui_.scale();
    const float x0 = 16 * S, W = ImGui::GetWindowWidth() - 32 * S, bw = 110 * S, gap = 8 * S;
    ImGui::SetCursorPos(ImVec2(x0, y));
    nxui::colored_text(nxui::col::accent, label);
    y += 22 * S;
    ImGui::SetCursorPos(ImVec2(x0, y));
    ImGui::BeginDisabled(running_);
    const float ew = W - bw - gap - (extra_label ? extra_btn_w + gap : 0);
    if (editable) nxui::input_text(id, text, ew);
    else nxui::text_box(id, *text, ew);
    if (extra_label) {
        ImGui::SetCursorPos(ImVec2(x0 + ew + gap, y));
        if (nxui::button(extra_label, ImVec2(extra_btn_w, 0))) extra();
    }
    ImGui::SetCursorPos(ImVec2(x0 + W - bw, y));
    if (nxui::button("Browse...", ImVec2(bw, 0))) browse();
    ImGui::EndDisabled();
    y += 32 * S;
}

void App::frame() {
    mail_.drain();
    if (debounce_at_ms_ && SDL_GetTicks() >= debounce_at_ms_) {
        debounce_at_ms_ = 0;
        request_inspect();
    }
    if (running_) refresh_progress();
    ui_.keep_awake(running_ || inspect_running_ || debounce_at_ms_ != 0);

    const float S = ui_.scale();
    const float x0 = 16 * S, W = ImGui::GetWindowWidth() - 32 * S, H = ImGui::GetWindowHeight();
    float y = 12 * S;

    std::string dump_shown = dump_path_;
    draw_path_row(y, "Your game dump", "##dump", &dump_shown, false, 70 * S, "Clear", [this] { set_dump(""); }, [this] { browse_dump(); });
    ImGui::SetCursorPos(ImVec2(x0, y - 2 * S));
    ImGui::PushTextWrapPos(x0 + W);
    nxui::colored_text(dump_status_color_, dump_status_, true);
    ImGui::PopTextWrapPos();
    y += 36 * S - 2 * S;

    draw_path_row(y, "Your Switch keys", "##keys", &keys_text_, true, 0, nullptr, {}, [this] { browse_keys(); });
    if (keys_text_ != prev_keys_) keys_or_out_edited();
    ImGui::SetCursorPos(ImVec2(x0, y - 2 * S));
    ImGui::PushTextWrapPos(x0 + W);
    nxui::hint(keys_hint_);
    ImGui::PopTextWrapPos();
    y += 36 * S - 2 * S;

    draw_path_row(y, "Install to", "##out", &out_text_, true, 0, nullptr, {}, [this] { browse_folder(); });
    if (out_text_ != prev_out_) keys_or_out_edited();
    ImGui::SetCursorPos(ImVec2(x0, y - 2 * S));
    nxui::hint(disk_line_);
    y += 26 * S;

    ImGui::SetCursorPos(ImVec2(x0, y));
    nxui::colored_text(nxui::col::accent, "Texture quality");
    y += 22 * S;
    ImGui::SetCursorPos(ImVec2(x0, y));
    ImGui::BeginDisabled(running_);
    ImGui::SetNextItemWidth(260 * S);
    if (ImGui::BeginCombo("##quality", quality_items_[static_cast<size_t>(quality_sel_)].c_str())) {
        for (int i = 0; i < static_cast<int>(quality_items_.size()); ++i)
            if (ImGui::Selectable(quality_items_[static_cast<size_t>(i)].c_str(), i == quality_sel_)) quality_sel_ = i;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SetCursorPos(ImVec2(x0 + 272 * S, y + 2 * S));
    ImGui::PushTextWrapPos(x0 + W);
    nxui::hint("Converts the game's textures once so they load fast. About 3 to 12 minutes.");
    ImGui::PopTextWrapPos();
    y += 36 * S;

    // buttons
    ImGui::SetCursorPos(ImVec2(x0, y));
    const bool can = running_ ? !stop_requested_ : can_start();
    ImGui::BeginDisabled(!can);
    if (nxui::primary_button(running_ ? (stop_requested_ ? "Stopping..." : "Stop") : "Start setup", ImVec2(150 * S, 0))) {
        if (running_) request_stop();
        else start_run();
    }
    ImGui::EndDisabled();
    if (run_success_) {
        ImGui::SetCursorPos(ImVec2(x0 + 160 * S, y));
        if (nxui::button("Open folder", ImVec2(130 * S, 0))) nxui::App::open_path(out_text());
        ImGui::SetCursorPos(ImVec2(x0 + 300 * S, y));
        if (nxui::button("Open log", ImVec2(110 * S, 0))) nxui::App::open_path(out_text() + "/setup.log");
    }
    ImGui::SetCursorPos(ImVec2(x0 + W - 110 * S, y));
    if (nxui::button("Close", ImVec2(110 * S, 0))) {
        if (running_) {
            request_stop();
            close_after_run_ = true;
        } else {
            ui_.quit(exit_code_);
        }
    }
    y += 38 * S;

    // progress
    ImGui::SetCursorPos(ImVec2(x0, y));
    ImGui::PushTextWrapPos(x0 + W);
    ImGui::PushStyleColor(ImGuiCol_Text, nxui::col::accent);
    ImGui::TextUnformatted(stage_title_.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    y += 20 * S;
    ImGui::SetCursorPos(ImVec2(x0, y));
    nxui::colored_text(nxui::col::gray, stage_detail_);
    y += 20 * S;
    ImGui::SetCursorPos(ImVec2(x0, y));
    ImGui::ProgressBar(stage_frac_, ImVec2(W, 14 * S), "");
    y += 20 * S;
    ImGui::SetCursorPos(ImVec2(x0, y));
    nxui::colored_text(nxui::col::gray, overall_text_);
    y += 20 * S;
    ImGui::SetCursorPos(ImVec2(x0, y));
    ImGui::ProgressBar(overall_frac_, ImVec2(W, 12 * S), "");
    y += 18 * S;

    // the log
    ImGui::SetCursorPos(ImVec2(x0, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, nxui::col::rgb(0xFAFAFA));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * S, 4 * S));
    if (ImGui::BeginChild("##log", ImVec2(W, std::max(60 * S, H - 16 * S - y)), ImGuiChildFlags_Borders)) {
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(log_.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) ImGui::TextUnformatted(log_[static_cast<size_t>(i)].c_str());
        if (log_dirty_) {
            ImGui::SetScrollHereY(1.0f);
            log_dirty_ = false;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

int App::run() {
    nxui::Config uc;
    const char* v = NXS_VERSION;
    uc.title = std::string(nxs::game().product) + " - Setup  " + (std::isdigit(static_cast<unsigned char>(v[0])) ? "v" : "") + v;
    uc.width = kLogicalW;
    uc.height = kLogicalH;
    if (!ui_.init(uc)) {
        std::fprintf(stderr, "%s\n", ui_.error().c_str());
        return static_cast<int>(Fail::Generic);
    }

    // the log window shows what the pipeline logs (from whichever thread: the line goes through the mailbox)
    nxs::log_set_sink(
        [this](LogLevel, const std::string& text) {
            mail_.post([this, text] { append_log(text); });
            ui_.wake();
        },
        LogLevel::Info);

    out_text_ = cli_.out.empty() ? exe_folder() + "/game" : cli_.out;
    prev_out_ = out_text_;
    dump_path_ = trim_path(cli_.input);
    quality_items_ = {"Balanced (recommended)", "Faster, lower quality", "Best, slower"};
    quality_values_ = {3, 1, 5};
    if (!cli_.keys.empty()) {
        keys_text_ = cli_.keys[0];
        prev_keys_ = keys_text_;
        extra_keys_.assign(cli_.keys.begin() + 1, cli_.keys.end());
        update_keys_hint();
    } else {
        const auto found = nxs::find_key_files();
        if (!found.empty()) set_keys(found[0].path, found[0].source);
    }
    if (cli_.quality >= 0) {
        int index = -1;
        for (size_t i = 0; i < quality_values_.size(); ++i)
            if (quality_values_[i] == cli_.quality) index = static_cast<int>(i);
        if (index < 0) {
            quality_items_.push_back(std::format("Custom (quality {})", cli_.quality));
            quality_values_.push_back(cli_.quality);
            index = static_cast<int>(quality_values_.size()) - 1;
        }
        quality_sel_ = index;
    }
    auto_start_pending_ = cli_.auto_start;
    request_inspect();

    nxui::App::Hooks h;
    h.frame = [this] { frame(); };
    h.on_event = [this](const SDL_Event& ev) { return on_event(ev); };
    h.can_close = [this] {
        if (running_) {
            request_stop();
            close_after_run_ = true;
            return false;
        }
        return true;
    };
    const int rc = ui_.run(h);
    nxs::log_set_sink({}, LogLevel::Info);
    if (inspect_thread_ && inspect_thread_->joinable()) inspect_thread_->join();
    if (run_thread_ && run_thread_->joinable()) {
        shared_.cancel = true;
        run_thread_->join();
    }
    nxs::log_close();
    return rc != 0 ? rc : exit_code_;
}

}  // namespace

int main(int argc, char** argv) {
    App app(parse_command_line(argc, argv));
    return app.run();
}
