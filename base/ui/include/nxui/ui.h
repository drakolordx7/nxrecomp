// SPDX-License-Identifier: GPL-3.0-or-later
// nxui: the one UI shell of the NX programs that have a window of their own (the launcher, the setup tool). SDL3 for the window, input,
// file dialogs and the renderer; Dear ImGui for the widgets. The same code runs on Windows and Linux: nothing in here names an OS.
//
// An App owns the window and the render loop. The program gives it a frame() function that draws one frame with ImGui; the App adds
// the parts every program needs: the DPI scale (S(px) turns a logical pixel, 96 DPI, into the window's), the fonts (DejaVu Sans, the
// repository's own font, plus a system font for CJK text when one is installed), the light theme, modal message boxes, file and
// folder pickers, and a way for worker threads to wake the loop.
//
// Test aid: with the environment variable NXUI_SHOT=<file.bmp> the App saves its window to that file after a few frames and ends the
// loop (NXUI_SHOT_FRAMES=<n> changes how many).
#pragma once
#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
union SDL_Event;

namespace nxui {

// The colours of the theme (light). Status colours are the ones the Win32 launcher used.
namespace col {
inline ImVec4 rgb(unsigned v, float a = 1.f) { return ImVec4(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a); }
inline const ImVec4 gray = rgb(0x5F5F5F);      // hints and notes
inline const ImVec4 green = rgb(0x0F6B2F);     // ready
inline const ImVec4 red = rgb(0xB31B1B);       // a problem
inline const ImVec4 amber = rgb(0x8A5A00);     // attention
inline const ImVec4 accent = rgb(0x0B5CAD);    // headings, the primary button, the selected tab
}  // namespace col

struct Config {
    std::string title;          // the window title
    int width = 680;            // client size in logical pixels (96 DPI)
    int height = 650;
    bool resizable = false;
    std::string app_id;         // the desktop application id (Wayland and X11 class), optional
};

// One button of a message box and the answer it gives (its index).
struct MessageSpec {
    std::string title;
    std::string text;
    std::vector<std::string> buttons;     // empty: one "OK"
    std::function<void(int)> done;        // the index of the button pressed (Escape and the close box answer the last button)
    ImVec4 color = ImVec4(0, 0, 0, 0);    // text colour (alpha 0: the theme's)
};

class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Opens SDL, the window and the renderer and sets up ImGui. False (with error()) when that cannot be done.
    bool init(const Config& cfg);
    const std::string& error() const { return error_; }

    float scale() const { return scale_; }                      // the UI scale: logical pixels to window pixels
    float S(float px) const { return px * scale_; }
    ImVec2 S(float x, float y) const { return ImVec2(x * scale_, y * scale_); }
    SDL_Window* window() const { return window_; }
    void set_title(const std::string& title);

    // The render loop. `frame` draws the content of the window (inside a window that fills it: margins are the program's). `on_event`
    // sees every SDL event first; return true to keep ImGui from seeing it. Returns when quit() was called or the window was closed and
    // `can_close` (when given) agreed; the return value is the exit code given to quit().
    struct Hooks {
        std::function<void()> frame;
        std::function<bool(const SDL_Event&)> on_event;
        std::function<bool()> can_close;          // the player closed the window: true = end (default: yes)
        std::function<void()> on_scale_changed;   // after the DPI scale changed (sizes taken from S() are stale)
    };
    int run(const Hooks& hooks);
    void quit(int exit_code = 0);                // thread-safe
    void wake();                                 // thread-safe: run one more frame soon (a worker has news)
    void keep_awake(bool on) { busy_ = on; }     // frames at 20 Hz while on (progress bars), else only on input

    // Modal dialogs, drawn by the loop on top of the frame. They return at once; `done` runs on a later frame.
    void message(MessageSpec spec);
    bool modal_open() const;

    // File pickers (SDL's native dialogs; where the system has none, a box to type the path in). `done` runs on the UI thread with the
    // chosen paths (UTF-8), none when the player cancelled. filters: "Name|ext1;ext2" ("Switch dumps|nsp;xci;nsz;xcz"); the last may be "All files|*".
    void pick_files(const std::string& title, const std::vector<std::string>& filters, bool many, const std::string& start,
                    std::function<void(std::vector<std::string>)> done);
    void pick_folder(const std::string& title, const std::string& start, std::function<void(std::string)> done);

    // Opens a file or folder with the system (the default program / file manager). False when that failed.
    static bool open_path(const std::string& path);

    struct Impl;                                 // (internal)

private:
    Impl* d_ = nullptr;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::string error_;
    float scale_ = 1.f;
    bool busy_ = false;
};

// ---- widgets shared by the programs ------------------------------------------------------------------------------------------------

void heading(const char* text);                                            // accent text with a rule under it
void hint(const std::string& text, float wrap_width = 0.f);                // grey wrapped text
void colored_text(const ImVec4& c, const std::string& text, bool wrapped = false);
bool primary_button(const char* label, const ImVec2& size = ImVec2(0, 0));  // filled with the accent colour
bool button(const char* label, const ImVec2& size = ImVec2(0, 0));          // a plain button (disabled look from BeginDisabled)
// A read-only single-line box that shows (the start of) a path or other text.
void text_box(const char* id, const std::string& text, float width);
// An editable single-line box on a std::string. True when the text changed.
bool input_text(const char* id, std::string* text, float width, const char* hint_text = nullptr);

// Text of the file name with the folder cut: "C:/a/b/c.nsp" -> "c.nsp" (for either separator).
std::string base_name(const std::string& path);

}  // namespace nxui
