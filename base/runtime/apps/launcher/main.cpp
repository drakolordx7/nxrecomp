// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher: a small window that opens before the game. It edits controls.ini in the game folder (display, picture, mouse,
// keyboard, controller, button prompts, frame rate counter), shows whether the game files are in place, starts the setup tool
// for the first-time setup, and starts the game exe (<id>).
//   <id>_launcher [--game <folder>] [--page <n>] [--auto-play] [--auto-prepare] [--no-prepare] [--game-args "<text>"] [--setup-args "<text>"]
//                 [--auto-save] [--auto-report <file>] [--auto-press setup|play]
// --game uses that game folder for this run (the folder saved in <id>_launcher.ini is left alone). --page opens that tab.
// --auto-play saves nothing, opens no window and starts the game at once (for scripted runs). --game-args adds text to
// the command line of the game exe; --setup-args does the same for the setup tool (scripted runs).
// --auto-prepare runs the shader preparation without asking: at startup when a window is open, and before the game when
// --auto-play is given (then it waits for it). --no-prepare never asks and never starts the preparation on its own; the
// Extras tab button still works.
// Automation for scripted runs (the test scripts and the release smoke gate), the same on every system:
// --auto-save opens no window: it loads controls.ini through the pages and saves it back, exactly what Save does (exit 1 when that fails).
// --auto-report <file> opens no window: it writes what the launcher knows about the game folder to <file> as key=value lines (state=ready
// or not_ready, summary, settings=missing|ok|broken, pages, ...). It runs after --auto-save when both are given.
// --auto-press setup|play opens the window and presses that button once on the first frame, as a click would.
// NXUI_SHOT=<file.bmp> (base/ui) saves a picture of the window and ends the run; --page picks the tab it shows.
//
// The same program on Windows and Linux: SDL3 and Dear ImGui (nxui), pages written against nxrt/game/launcher_ui.h.

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game_info.h"
#include "nxrt/game.h"
#include "nxrt/os/fs.h"
#include "nxrt/os/process.h"
#include "nxrt/os/time.h"
#include "nxui/ui.h"
#include "pad_capture.h"
#include "page_view.h"
#include "pages.h"

namespace nxl {
namespace {

using nxrt::play::default_settings;
using nxrt::play::InKind;
using nxrt::play::InputId;
using nxrt::play::load_settings;
using nxrt::play::save_settings;
namespace fs = nxrt::os::fs;

#ifndef NXRT_VERSION
#define NXRT_VERSION "dev"
#endif
constexpr int kLogicalW = 700;
constexpr int kLogicalH = 740;

// "v0.1.0" for a release build, "dev build" for a local one.
std::string version_text() {
    const char* v = NXRT_VERSION;
    return (v[0] >= '0' && v[0] <= '9') ? std::string("v") + v : std::string("dev build");
}

// Who started a shader preparation: decides what is said when it ends.
enum class PrepOrigin { automatic, play, extras };

std::string with_commas(unsigned long long v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

// Scripted runs (--auto-play): runs the shader preparation and waits for it. False, with *err, when it did not finish.
bool prepare_blocking(const std::string& dir, std::string* err) {
    PrepareRun run;
    if (!run.start(dir, true, nullptr, err)) return false;
    while (!run.finished()) nxrt::os::sleep_ms(50);
    run.finish();
    const PrepareLine r = run.result();
    if (r.kind == PrepareKind::ready || r.kind == PrepareKind::done) return true;
    *err = r.kind == PrepareKind::error ? r.text : std::string("Shader preparation did not finish.");
    return false;
}

struct CmdLine {
    std::string game;
    std::string game_args;
    std::string setup_args;
    int page = 0;
    bool auto_play = false;
    bool auto_prepare = false;
    bool no_prepare = false;
    bool auto_save = false;
    std::string auto_report;
    std::string auto_press;   // "setup" or "play"
};

CmdLine parse_cmdline(int argc, char** argv) {
    CmdLine c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--game" && more) c.game = argv[++i];
        else if (a == "--game-args" && more) c.game_args = argv[++i];
        else if (a == "--setup-args" && more) c.setup_args = argv[++i];
        else if (a == "--page" && more) c.page = std::atoi(argv[++i]);
        else if (a == "--auto-play") c.auto_play = true;
        else if (a == "--auto-prepare") c.auto_prepare = true;
        else if (a == "--no-prepare") c.no_prepare = true;
        else if (a == "--auto-save") c.auto_save = true;
        else if (a == "--auto-report" && more) c.auto_report = argv[++i];
        else if (a == "--auto-press" && more) c.auto_press = argv[++i];
    }
    return c;
}

class App : public Host {
public:
    explicit App(const CmdLine& cmd) : cmd_(cmd) {}
    int run();

    // Host
    void changed() override { dirty_ = true; }
    bool controller_connected() const override { return SDL_HasGamepad(); }
    bool shader_prepare_offered() const override { return shader_prepare_available(game_dir_); }
    bool shader_prepare_busy() const override { return setup_running_ || prep_.active(); }
    void prepare_shaders_now() override;
    void message(const std::string& text) override { say(text); }
    void ask(const std::string& text, std::vector<std::string> buttons, std::function<void(int)> done) override {
        ui_.message({product_name(), text, std::move(buttons), std::move(done)});
    }
    void capture_input(const std::string& what, std::function<void(bool, InputId)> done) override {
        cap_ = {};
        cap_.active = true;
        cap_.what = what;
        cap_.done = std::move(done);
    }
    void capture_pad_button(std::function<void(int)> done) override {
        padcap_ = {};
        padcap_.active = true;
        padcap_.done = std::move(done);
        pad_.open();   // if this fails the box just reports no controller; Escape still cancels
    }

private:
    struct InputCapture {
        bool active = false, opened = false;
        std::string what;
        std::function<void(bool, InputId)> done;
        ImVec2 min{0, 0}, max{0, 0};   // the box on screen, for the mouse clicks that count
    };
    struct PadCaptureState {
        bool active = false, opened = false;
        std::function<void(int)> done;
    };

    bool on_event(const SDL_Event& ev);
    void frame();
    void draw_header();
    void draw_tabs(float reserve_bottom);
    void draw_bottom();
    void draw_capture();
    void poll_jobs();

    void say(const std::string& text) { ui_.message({product_name(), text, {}, {}}); }
    void load_game_dir(const std::string& dir);
    void read_settings();
    int run_headless(const std::string& dir);
    bool write_report(const std::string& path);
    bool store_and_save(bool quiet);
    void refresh_status();
    void choose_folder();
    void run_setup();
    void play();
    void launch_game();
    void reset_defaults();
    bool confirm_close();
    void start_prepare(bool force, bool then_play, PrepOrigin origin);
    void on_prepare_finished();
    void cancel_prepare();
    void build_pages();
    bool busy() const { return setup_running_ || prep_.active(); }

    CmdLine cmd_;
    nxui::App ui_;
    std::vector<std::unique_ptr<Page>> pages_;
    int want_page_ = -1;               // --page: select this tab on the first frame
    bool press_pending_ = false;       // --auto-press: the button is pressed on the first frame

    std::string game_dir_;
    bool game_dir_pinned_ = false;     // --game: do not write the folder to <id>_launcher.ini
    GameStatus game_;
    Settings cur_;                     // the settings the pages edit (whole file, including what no page shows)
    bool settings_broken_ = false;     // controls.ini exists but could not be read
    bool dirty_ = false;
    std::string status_text_;
    ImVec4 status_color_ = nxui::col::gray;
    bool status_override_ = false;     // a message of the running setup replaces the folder summary
    bool setup_running_ = false;
    nxrt::os::process::Child setup_child_;
    bool setup_first_time_ = false;    // the folder was not ready when the running setup started
    uint64_t setup_poll_ms_ = 0;
    PrepareRun prep_;
    bool prep_showing_ = false;
    bool prep_then_play_ = false;      // start the game when this run ends well
    bool prep_declined_ = false;       // "No" to the Play question: not asked again in this session
    PrepOrigin prep_origin_ = PrepOrigin::automatic;
    std::string prep_text_;
    bool quitting_ = false;

    InputCapture cap_;
    PadCaptureState padcap_;
    PadCapture pad_;
};

void App::build_pages() {
    pages_.push_back(make_display_page());
    pages_.push_back(make_extras_page());
    pages_.push_back(make_controls_page());
    pages_.push_back(make_keys_page());
    pages_.push_back(make_pad_page());
    if (const GameLauncher* gl = nxrt::game::info().launcher)
        for (unsigned i = 0; i < gl->tab_count; ++i) pages_.push_back(gl->tabs[i]());
    for (auto& p : pages_) {
        p->attach(this);
        Form f(*p);
        p->build(f);
        p->load(cur_);
    }
}

void App::read_settings() {
    std::string err;
    Settings s;
    const bool ok = load_settings(controls_path(game_dir_), &s, &err);
    cur_ = s;
    settings_broken_ = !ok && !err.empty();
}

void App::load_game_dir(const std::string& dir) {
    game_dir_ = dir;
    read_settings();
    dirty_ = false;
    for (auto& p : pages_) p->load(cur_);
    refresh_status();
}

bool App::store_and_save(bool quiet) {
    for (auto& p : pages_) p->store(&cur_);
    const std::string path = controls_path(game_dir_);
    if (settings_broken_ && fs::exists(path)) {   // keep the unreadable file once, then replace it
        std::error_code ec;
        std::filesystem::copy_file(fs::native_path(path), fs::native_path(path + ".bak"), std::filesystem::copy_options::overwrite_existing, ec);
    }
    std::string err;
    if (!save_settings(path, cur_, &err)) {
        if (!quiet) say("The settings could not be saved:\n\n" + err);
        else std::fprintf(stderr, "The settings could not be saved: %s\n", err.c_str());
        return false;
    }
    settings_broken_ = false;
    dirty_ = false;
    refresh_status();
    return true;
}

void App::refresh_status() {
    game_ = check_game_dir(game_dir_);
    status_override_ = false;
    status_text_ = game_.summary();
    status_color_ = game_.ready() && game_.setup_finished ? nxui::col::green : (game_.ready() ? nxui::col::amber : nxui::col::red);
    if (settings_broken_) {
        status_text_ = "controls.ini could not be read, so the defaults are shown. Saving replaces it (a copy is kept as controls.ini.bak).";
        status_color_ = nxui::col::amber;
    }
}

void App::choose_folder() {
    if (busy()) return;
    ui_.pick_folder("Choose the game folder", game_dir_, [this](std::string path) {
        if (path.empty()) return;
        if (dirty_) store_and_save(true);   // the changes belong to the folder they were made in
        load_game_dir(path);
        if (!game_dir_pinned_) {
            LauncherConfig cfg;
            cfg.game_dir = game_dir_;
            std::string err;
            save_launcher_config(launcher_config_path(), cfg, &err);
        }
    });
}

void App::run_setup() {
    if (busy()) return;
    if (dirty_) store_and_save(true);
    setup_first_time_ = !game_.ready();
    std::string err;
    if (!start_setup(game_dir_, &setup_child_, &err, cmd_.setup_args)) {
        say(err.empty() ? "The setup tool could not be started." : err);
        return;
    }
    setup_running_ = true;
    status_override_ = true;
    status_text_ = "Setup is running. Finish it in the setup window; this window updates when it closes.";
    status_color_ = nxui::col::gray;
    setup_poll_ms_ = SDL_GetTicks();
}

void App::play() {
    if (!game_.ready()) {
        ask("The game files are not set up yet. Run setup now?", {"Yes", "No"}, [this](int a) {
            if (a == 0) run_setup();
        });
        return;
    }
    if (prep_.active()) return;
    if (!cmd_.no_prepare && !prep_declined_ && shader_prepare_available(game_dir_) && !shader_prepare_done(game_dir_)) {
        if (cmd_.auto_prepare) {
            start_prepare(false, true, PrepOrigin::play);
            return;
        }
        ask("Prepare shaders now? This takes a few minutes and only has to be done once (again after a graphics driver "
            "update). Without it the game stutters while it compiles shaders during the first hours of play.",
            {"Yes", "No", "Cancel"}, [this](int a) {
                if (a == 2) return;
                if (a == 0) {
                    start_prepare(false, true, PrepOrigin::play);
                    return;
                }
                prep_declined_ = true;   // No: start the game without it, and do not ask again in this session
                launch_game();
            });
        return;
    }
    launch_game();
}

void App::launch_game() {
    if (!store_and_save(false)) return;
    std::string err;
    if (!start_game(game_dir_, &err, cmd_.game_args)) {
        say(err.empty() ? "The game could not be started." : err);
        return;
    }
    quitting_ = true;
    ui_.quit(0);
}

void App::prepare_shaders_now() {
    if (busy()) return;
    if (!shader_prepare_available(game_dir_)) {
        say("This game folder has no shader list (shader_cache/programs.bin), so there is nothing to prepare. Run setup first.");
        return;
    }
    start_prepare(true, false, PrepOrigin::extras);
}

// Starts <id> --prepare-shaders (with --if-needed unless force) and shows the progress row. then_play: start the game when it
// ends well. The result is handled in on_prepare_finished().
void App::start_prepare(bool force, bool then_play, PrepOrigin origin) {
    if (busy()) return;
    std::string err;
    if (!prep_.start(game_dir_, !force, [this] { ui_.wake(); }, &err)) {
        say(err.empty() ? "Shader preparation could not be started." : err);
        return;
    }
    prep_then_play_ = then_play;
    prep_origin_ = origin;
    prep_showing_ = true;
    prep_text_ = "Preparing shaders...";
}

void App::on_prepare_finished() {
    prep_.finish();
    const bool cancelled = prep_.cancel_requested();
    const PrepareLine res = prep_.result();
    const bool then_play = prep_then_play_;
    const PrepOrigin origin = prep_origin_;
    prep_then_play_ = false;
    prep_showing_ = false;
    refresh_status();
    if (cancelled) return;   // the player stopped it; the run starts over the next time

    const bool finished = res.kind == PrepareKind::ready || res.kind == PrepareKind::done;
    const bool shaders_ready = res.kind == PrepareKind::ready || (res.kind == PrepareKind::done && res.failed == 0);
    if (!finished) {
        const std::string why = res.kind == PrepareKind::error ? res.text : std::string("The run ended without a result.");
        say("Shader preparation did not finish.\n\n" + why);
        return;
    }
    if (then_play) {
        launch_game();   // done with some programs left out still starts the game: those compile when needed
        return;
    }
    if (origin == PrepOrigin::extras) {
        if (shaders_ready) {
            say("Shaders are ready.");
        } else if (res.failed > 0) {
            say("Shader preparation finished, but " + with_commas(static_cast<unsigned long long>(res.failed)) +
                " programs could not be prepared. The game compiles those when it needs them.");
        } else {
            say("Shader preparation finished.");
        }
    }
}

void App::cancel_prepare() {
    if (!prep_.active()) return;
    prep_.cancel();
    prep_text_ = "Cancelling...";
}

void App::reset_defaults() {
    ask("Reset every setting, key binding and controller button to the defaults? Nothing is saved until you press Save or Play.", {"OK", "Cancel"},
        [this](int a) {
            if (a != 0) return;
            cur_ = default_settings();
            for (auto& p : pages_) p->load(cur_);
            dirty_ = true;
        });
}

// The window's close box: true when the window may close now. A question is asked asynchronously; its answer closes the window.
bool App::confirm_close() {
    if (quitting_) return true;
    if (setup_running_) {
        say("Setup is still running. Close its window first.");
        return false;
    }
    if (prep_.active()) {
        say("Shader preparation is still running. Cancel it first, or wait until it finishes.");
        return false;
    }
    if (!dirty_) return true;
    ask("Save your changes before closing?", {"Yes", "No", "Cancel"}, [this](int a) {
        if (a == 2) return;
        if (a == 0 && !store_and_save(false)) return;
        quitting_ = true;
        ui_.quit(0);
    });
    return false;
}

// Runs once per frame: the setup process and the shader preparation.
void App::poll_jobs() {
    if (setup_running_ && SDL_GetTicks() - setup_poll_ms_ >= 500) {
        setup_poll_ms_ = SDL_GetTicks();
        int code = 0;
        if (setup_child_.wait(0, &code)) {
            setup_child_.close();
            setup_running_ = false;
            refresh_status();
            SDL_RaiseWindow(ui_.window());
            // first-time setup that finished: prepare the shaders now, so the first play session has no stutter
            if (setup_first_time_ && !cmd_.no_prepare && game_.ready() && game_.setup_finished && shader_prepare_available(game_dir_) &&
                !shader_prepare_done(game_dir_))
                start_prepare(false, false, PrepOrigin::automatic);
        }
    }
    if (prep_.active()) {
        if (prep_.finished()) {
            on_prepare_finished();
        } else if (!prep_.cancel_requested()) {
            const PrepareLine p = prep_.progress();
            if (p.kind == PrepareKind::progress && prep_showing_)
                prep_text_ = "Preparing shaders: " + with_commas(p.done) + " of " + with_commas(p.total);
        }
    }
}

// ---- events and frames -----------------------------------------------------------------------------------------------------------------

bool App::on_event(const SDL_Event& ev) {
    if (padcap_.active) {
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.scancode == SDL_SCANCODE_ESCAPE) {
            padcap_.active = false;
            pad_.close();
            if (padcap_.done) padcap_.done(-1);
            return true;
        }
        const int row = pad_.on_event(ev);
        if (row >= 0) {
            padcap_.active = false;
            pad_.close();
            if (padcap_.done) padcap_.done(row);
            return true;
        }
        return ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP || ev.type == SDL_EVENT_TEXT_INPUT;
    }
    if (!cap_.active) return false;
    auto take = [&](InKind kind, int code) {
        cap_.active = false;
        auto done = std::move(cap_.done);
        if (done) done(true, InputId{kind, code});
        return true;
    };
    switch (ev.type) {
        case SDL_EVENT_KEY_DOWN:
            if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                cap_.active = false;
                auto done = std::move(cap_.done);
                if (done) done(false, InputId{});
                return true;
            }
            if (ev.key.repeat) return true;
            if (ev.key.scancode != SDL_SCANCODE_UNKNOWN) return take(InKind::Key, static_cast<int>(ev.key.scancode));
            return true;
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            // a click inside the box binds that mouse button (SDL numbers them 1 left, 2 middle, 3 right, 4 and 5 the side buttons)
            const float x = ev.button.x, y = ev.button.y;
            if (x >= cap_.min.x && x <= cap_.max.x && y >= cap_.min.y && y <= cap_.max.y && ev.button.button >= 1 && ev.button.button <= 5)
                return take(InKind::Mouse, ev.button.button);
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
            return true;
        case SDL_EVENT_MOUSE_WHEEL: {
            float y = ev.wheel.y;
            if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
            if (y > 0) return take(InKind::Wheel, 1);
            if (y < 0) return take(InKind::Wheel, -1);
            return true;
        }
        default:
            return false;
    }
}

void App::draw_capture() {
    const float S = ui_.scale();
    if (cap_.active && !cap_.opened) {
        ImGui::OpenPopup("Press an input###cap");
        cap_.opened = true;
    }
    if (cap_.opened) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(400 * S, 150 * S));
        if (ImGui::BeginPopupModal("Press an input###cap", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            if (!cap_.active) {
                ImGui::CloseCurrentPopup();
                cap_.opened = false;
            } else {
                cap_.min = ImGui::GetWindowPos();
                cap_.max = ImVec2(cap_.min.x + ImGui::GetWindowWidth(), cap_.min.y + ImGui::GetWindowHeight());
                ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 12 * S);
                ImGui::SetCursorPos(ImVec2(12 * S, ImGui::GetCursorPosY() + 4 * S));
                ImGui::TextUnformatted("Press the key or mouse button for");
                ImGui::SetCursorPosX(12 * S);
                nxui::colored_text(nxui::col::accent, cap_.what);
                ImGui::SetCursorPosX(12 * S);
                nxui::hint("Click inside this window to use a mouse button. Escape cancels.");
                ImGui::PopTextWrapPos();
            }
            ImGui::EndPopup();
        } else {
            cap_.opened = false;
        }
    }
    if (padcap_.active && !padcap_.opened) {
        ImGui::OpenPopup("Find a button###padcap");
        padcap_.opened = true;
    }
    if (padcap_.opened) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(400 * S, 150 * S));
        if (ImGui::BeginPopupModal("Find a button###padcap", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            if (!padcap_.active) {
                ImGui::CloseCurrentPopup();
                padcap_.opened = false;
            } else {
                ImGui::SetCursorPos(ImVec2(12 * S, ImGui::GetCursorPosY() + 10 * S));
                ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 12 * S);
                ImGui::TextUnformatted(pad_.has_pad() ? "Press the controller button you want to change. Escape cancels."
                                                       : "No controller found. Connect one and press a button, or press Escape.");
                ImGui::PopTextWrapPos();
            }
            ImGui::EndPopup();
        } else {
            padcap_.opened = false;
        }
    }
}

void App::draw_header() {
    const float S = ui_.scale();
    const float W = ImGui::GetWindowWidth();
    const float m = 16 * S;
    ImGuiStyle& st = ImGui::GetStyle();
    // title and version
    ImGui::SetCursorPos(ImVec2(m, 8 * S));
    ImGui::PushFont(nullptr, st.FontSizeBase * 1.45f);
    ImGui::TextUnformatted(product_name().c_str());
    ImGui::PopFont();
    const std::string ver = version_text();
    const float vw = ImGui::CalcTextSize(ver.c_str()).x;
    ImGui::SetCursorPos(ImVec2(W - m - vw, 18 * S));
    nxui::colored_text(nxui::col::gray, ver);
    // status
    ImGui::SetCursorPos(ImVec2(m, 44 * S));
    nxui::colored_text(status_color_, status_text_);
    // game folder
    const float row = 72 * S;
    ImGui::SetCursorPos(ImVec2(m, row + 4 * S));
    ImGui::TextUnformatted("Game folder");
    const float bw_change = 90 * S, bw_setup = 160 * S, gap = 8 * S;
    const float fx = m + 88 * S, right = W - m;
    ImGui::SetCursorPos(ImVec2(fx, row));
    nxui::text_box("##folder", game_dir_, right - bw_setup - bw_change - 2 * gap - fx);
    ImGui::BeginDisabled(busy());
    ImGui::SetCursorPos(ImVec2(right - bw_setup - gap - bw_change, row));
    if (nxui::button("Change...", ImVec2(bw_change, 0))) choose_folder();
    ImGui::SetCursorPos(ImVec2(right - bw_setup, row));
    if (nxui::button(game_.ready() ? "Run setup again..." : "Set up game files...", ImVec2(bw_setup, 0))) run_setup();
    ImGui::EndDisabled();
}

void App::draw_tabs(float reserve_bottom) {
    const float S = ui_.scale();
    const float m = 16 * S;
    const float W = ImGui::GetWindowWidth();
    const float top = 112 * S;
    ImGui::SetCursorPos(ImVec2(m, top));
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginChild("##tabs", ImVec2(W - 2 * m, ImGui::GetWindowHeight() - top - reserve_bottom), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTabBar("##t", ImGuiTabBarFlags_None)) {
            for (size_t i = 0; i < pages_.size(); ++i) {
                ImGuiTabItemFlags fl = 0;
                if (want_page_ == static_cast<int>(i)) fl |= ImGuiTabItemFlags_SetSelected;
                if (ImGui::BeginTabItem(pages_[i]->title(), nullptr, fl)) {
                    draw_page(*pages_[i], ui_, *this);
                    ImGui::EndTabItem();
                }
            }
            want_page_ = -1;
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    (void)st;
}

void App::draw_bottom() {
    const float S = ui_.scale();
    const float m = 16 * S;
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    const float right = W - m, gap = 8 * S;
    // shader preparation progress row
    if (prep_showing_) {
        const float py = H - 80 * S;
        ImGui::SetCursorPos(ImVec2(m, py + 4 * S));
        ImGui::TextUnformatted(prep_text_.c_str());
        const float bar_x = m + 300 * S + gap;
        const PrepareLine p = prep_.progress();
        const float frac = p.kind == PrepareKind::progress && p.total > 0 ? static_cast<float>(std::min(p.done, p.total)) / static_cast<float>(p.total) : 0.f;
        ImGui::SetCursorPos(ImVec2(bar_x, py + 6 * S));
        ImGui::ProgressBar(frac, ImVec2(right - 100 * S - gap - bar_x, 18 * S), "");
        ImGui::SetCursorPos(ImVec2(right - 100 * S, py));
        ImGui::BeginDisabled(prep_.cancel_requested());
        if (nxui::button("Cancel", ImVec2(100 * S, 0))) cancel_prepare();
        ImGui::EndDisabled();
    }
    const float by = H - 42 * S;
    ImGui::SetCursorPos(ImVec2(m, by));
    ImGui::BeginDisabled(setup_running_);
    if (nxui::button("Reset to defaults", ImVec2(140 * S, 0))) reset_defaults();
    ImGui::EndDisabled();
    const char* note = setup_running_ ? "Setup is running in its own window." : (prep_.active() ? "Shader preparation is running." : (dirty_ ? "Unsaved changes" : ""));
    ImGui::SetCursorPos(ImVec2(m + 156 * S, by + 4 * S));
    nxui::colored_text(nxui::col::gray, note);
    ImGui::SetCursorPos(ImVec2(right - 100 * S - gap - 90 * S, by));
    ImGui::BeginDisabled(!dirty_ || busy());
    if (nxui::button("Save", ImVec2(90 * S, 0))) store_and_save(false);
    ImGui::EndDisabled();
    ImGui::SetCursorPos(ImVec2(right - 100 * S, by));
    ImGui::BeginDisabled(!game_.ready() || busy());
    if (nxui::primary_button("Play", ImVec2(100 * S, 0))) play();
    ImGui::EndDisabled();
}

void App::frame() {
    if (press_pending_) {
        press_pending_ = false;
        if (cmd_.auto_press == "setup") run_setup();
        else play();
    }
    poll_jobs();
    ui_.keep_awake(busy() || cap_.active || padcap_.active);
    for (auto& p : pages_) p->state_changed();
    const float S = ui_.scale();
    const float reserve = (56 + (prep_showing_ ? 32 : 0)) * S;
    draw_header();
    draw_tabs(reserve);
    draw_bottom();
    draw_capture();
}

// --auto-save / --auto-report: no window. The pages are built and loaded the way the window does it, so a save here is what Save writes.
int App::run_headless(const std::string& dir) {
    game_dir_ = dir;
    game_dir_pinned_ = true;
    read_settings();
    build_pages();
    refresh_status();
    int rc = 0;
    if (cmd_.auto_save && !store_and_save(true)) rc = 1;
    if (!cmd_.auto_report.empty() && !write_report(cmd_.auto_report)) rc = 1;
    return rc;
}

bool App::write_report(const std::string& path) {
    const char* settings = settings_broken_ ? "broken" : (fs::exists(controls_path(game_dir_)) ? "ok" : "missing");
    std::string t;
    auto line = [&](const char* k, const std::string& v) { t += std::string(k) + "=" + v + "\n"; };
    auto flag = [](bool b) { return std::string(b ? "1" : "0"); };
    line("version", version_text());
    line("product", product_name());
    line("game_dir", game_dir_);
    line("state", game_.ready() ? "ready" : "not_ready");
    line("setup_finished", flag(game_.setup_finished));
    line("exefs", flag(game_.exefs));
    line("romfs", flag(game_.romfs));
    line("textures", flag(game_.textures));
    line("shaders", flag(game_.shaders));
    line("shader_prepare_available", flag(shader_prepare_available(game_dir_)));
    line("shader_prepare_done", flag(shader_prepare_done(game_dir_)));
    line("settings", settings);
    line("pages", std::to_string(pages_.size()));
    line("summary", game_.summary());
    std::FILE* f = fs::fopen(path, "wb");
    if (!f) {
        std::fprintf(stderr, "The report file could not be written: %s\n", path.c_str());
        return false;
    }
    const bool ok = std::fwrite(t.data(), 1, t.size(), f) == t.size();
    return std::fclose(f) == 0 && ok;
}

int App::run() {
    LauncherConfig cfg;
    load_launcher_config(launcher_config_path(), &cfg);
    std::string dir = cfg.game_dir.empty() ? default_game_dir() : cfg.game_dir;
    if (!cmd_.game.empty()) {
        dir = cmd_.game;
        game_dir_pinned_ = true;
    }

    if (cmd_.auto_save || !cmd_.auto_report.empty()) return run_headless(dir);

    if (cmd_.auto_play) {
        SDL_Init(0);
        std::string err;
        if (cmd_.auto_prepare && !cmd_.no_prepare && shader_prepare_available(dir) && !shader_prepare_done(dir) && !prepare_blocking(dir, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        if (!start_game(dir, &err, cmd_.game_args)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        return 0;
    }

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    nxui::Config uc;
    uc.title = product_name() + "  " + version_text();
    uc.width = kLogicalW;
    uc.height = kLogicalH;
    if (!ui_.init(uc)) {
        std::fprintf(stderr, "%s\n", ui_.error().c_str());
        nxrt::os::process::error_box(product_name().c_str(), ui_.error().c_str());
        return 1;
    }
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);   // for the "Controller: Connected" line; the pad box opens it again (counted)

    game_dir_ = dir;
    read_settings();
    build_pages();
    want_page_ = std::clamp(cmd_.page, 0, static_cast<int>(pages_.size()) - 1);
    press_pending_ = cmd_.auto_press == "setup" || cmd_.auto_press == "play";
    refresh_status();
    if (cmd_.auto_prepare && !cmd_.no_prepare && shader_prepare_available(game_dir_) && !shader_prepare_done(game_dir_))
        start_prepare(false, false, PrepOrigin::automatic);

    nxui::App::Hooks h;
    h.frame = [this] { frame(); };
    h.on_event = [this](const SDL_Event& ev) { return on_event(ev); };
    h.can_close = [this] { return confirm_close(); };
    const int rc = ui_.run(h);
    prep_.cancel();
    return rc;
}

}  // namespace
}  // namespace nxl

#ifdef NXRT_LAUNCHER_GAME_REGISTER
void NXRT_LAUNCHER_GAME_REGISTER();     // the data part of the game module the launcher is built for (cmake/launcher.cmake)
#endif

int main(int argc, char** argv) {
#ifdef NXRT_LAUNCHER_GAME_REGISTER
    NXRT_LAUNCHER_GAME_REGISTER();
#endif
    nxrt::os::process::attach_parent_console();
    nxl::App app(nxl::parse_cmdline(argc, argv));
    return app.run();
}
