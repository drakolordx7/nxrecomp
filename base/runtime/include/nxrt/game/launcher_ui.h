// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's page API, the same on every system: a page describes its rows once (labelled check boxes, drop-downs, sliders, number boxes,
// buttons, texts, colour swatches) with a Form; the launcher draws them (SDL3 + Dear ImGui, apps/launcher) and the Form remembers how each row
// reads from and writes to a Settings copy, so a page rarely needs more than a few lines per setting. There is no window-system type in here:
// a game's pages are written once and show on Windows and Linux alike. All text is UTF-8.
//
// What a game adds to the launcher (nxrt/game.h Info::launcher): tabs after the base's own and rows at the end of the Extras tab (GameLauncher).
// The pages are compiled into the launcher exe by the game module (its launcher library).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/game/play_types.h"

namespace nxl {

using nxrt::play::Settings;

class Page;

// A row of a page. The page's code reaches a row through the pointer its Form call returned: to read what the player chose, or to change the
// row (its value, text, drop-down items, enabled and visible state). The pointer stays valid as long as the page.
struct Row {
    enum class Kind { Heading, Hint, Gap, Check, Combo, Slider, Number, Button, Text, Swatch, Custom };

    // ---- for the page's code
    int value() const { return value_; }     // check box 0/1, drop-down index, slider position, number as typed
    void set_value(int v) {                  // not a change by the player: nothing is marked unsaved
        value_ = v;
        if (kind == Kind::Number) number_buf = std::to_string(v);
    }
    void set_text(std::string t) { text = std::move(t); }   // Text rows: the shown text
    void set_items(std::vector<std::string> items) { this->items = std::move(items); }   // Combo rows
    void set_enabled(bool e) { enabled = e; }
    void set_visible(bool v) { visible = v; }
    void flash() { flash_frames = 180; }     // draws the row highlighted for a moment and scrolls it into view

    // ---- the launcher's (drawing and layout); a page does not touch these
    Kind kind = Kind::Gap;
    std::string label;                       // left of the control (Check: the text next to the box)
    std::string text;                        // Hint, Heading, Text value, Button caption, Number suffix
    std::vector<std::string> items;
    int lo = 0, hi = 0;
    int value_ = 0;
    std::string number_buf;                  // Number: what is typed
    int width = 0;                           // Number and Button: logical px
    int cols = 1, col = 0;                   // the grid the row sits in (Form::columns)
    bool compact = false;
    bool full_width = false;                 // Hint: from the left margin instead of the control column
    bool at_left = false;                    // Button: at the left of its cell instead of the control column
    bool joined = false;                     // Number: on the line of the row before, behind an "x"
    bool enabled = true, visible = true;
    int button_id = 0;
    int gap_px = 0;
    int flash_frames = 0;
    std::function<void(const Settings&)> load;
    std::function<void(Settings&)> store;
    std::function<std::string(int)> format;  // Slider: the text for a position
    std::function<void()> after;             // the player changed it
    std::function<uint32_t()> rgb;           // Swatch: 0xRRGGBB
    std::function<void(float)> draw;         // Custom: ImGui calls (launcher-internal pages only); the argument is the width available
};

// What a page can ask of the launcher.
class Host {
public:
    virtual ~Host() = default;
    virtual void changed() = 0;                           // the player changed a control: unsaved changes
    virtual bool controller_connected() const = 0;        // a game controller is connected
    virtual bool shader_prepare_offered() const = 0;      // the game folder has a shader list (programs.bin)
    virtual bool shader_prepare_busy() const = 0;         // setup or shader preparation is running
    virtual void prepare_shaders_now() = 0;               // run the full shader preparation now (Extras button)
    // A box with one OK button. Returns at once.
    virtual void message(const std::string& text) = 0;
    // A box with these buttons; `done` gets the index of the one pressed, later.
    virtual void ask(const std::string& text, std::vector<std::string> buttons, std::function<void(int)> done) = 0;
    // "Press a key or click a mouse button": `done(true, input)` with what was pressed, `done(false, {})` when cancelled. `what` names the thing
    // being bound ("Jump").
    virtual void capture_input(const std::string& what, std::function<void(bool, nxrt::play::InputId)> done) = 0;
    // "Press the controller button you want to change": `done` gets the row of the Controller tab (pad_capture.h) or -1 when cancelled.
    virtual void capture_pad_button(std::function<void(int)> done) = 0;
};

// Row builder. Rows go down the page in the order of the calls; columns(n) makes them fill left to right in a grid of n cells.
class Form {
public:
    using GetBool = std::function<bool(const Settings&)>;
    using SetBool = std::function<void(Settings&, bool)>;
    using GetInt = std::function<int(const Settings&)>;
    using SetInt = std::function<void(Settings&, int)>;
    using After = std::function<void()>;

    explicit Form(Page& page) : page_(page) {}

    void heading(std::string text);                                        // accent title with a rule, some space above
    Row* hint(std::string text, bool full_width = false);                  // grey text under the previous row (control column, or the page width)
    void gap(int px = 8);

    // One column (default) or two or three: rows then fill left to right. Switching back to 1 starts a new row.
    void columns(int n);
    // Narrow labels and slider values in multi-column rows (for pages that put several sliders side by side); off by default.
    void compact(bool on) { compact_ = on; }

    Row* checkbox(std::string text, GetBool get, SetBool set, After after = nullptr);
    // A drop-down list. get() returns the index of the item to show (-1: none).
    Row* combo(std::string label, std::vector<std::string> items, GetInt get, SetInt set, After after = nullptr);
    // A slider from lo to hi (integers); format() turns the position into the text shown next to it.
    Row* slider(std::string label, int lo, int hi, GetInt get, SetInt set, std::function<std::string(int)> format, After after = nullptr);
    // A single-line number box (clamped to lo..hi when stored).
    Row* number(std::string label, int lo, int hi, GetInt get, SetInt set, After after = nullptr, int width = 80);
    // Two number boxes on one line, "<a> x <b> <suffix>". Returns the first; the second follows it in the page (Page::row_after).
    Row* number_pair(std::string label, int lo, int hi, GetInt get_a, SetInt set_a, GetInt get_b, SetInt set_b, After after = nullptr,
                     std::string suffix = "");
    // A push button in the control column (or at the left of its cell). Returns its id: the page's command() gets it when pressed.
    int button(std::string label, int width = 150, bool at_left = false);
    // A static text row: label on the left, text on the right.
    Row* text_row(std::string label, std::string text);
    // A box filled with one colour (0xRRGGBB from rgb()) in the control column.
    Row* swatch(std::string label, std::function<uint32_t()> rgb, int height_px = 28);
    // Free drawing with Dear ImGui (for the base's own pages; a game's pages stay with the rows above).
    Row* custom(std::function<void(float)> draw);

private:
    Row* add(Row::Kind k);
    void place(Row* r);                                    // the grid cell of a control row
    Page& page_;
    int cols_ = 1, col_ = 0;
    bool compact_ = false;
    int next_button_ = 1;
};

class Page {
public:
    virtual ~Page();
    virtual const char* title() const = 0;                 // the tab's name
    virtual void build(Form& f) = 0;                       // describe the rows
    virtual void load(const Settings& s);                  // rows <- settings (default: every row the Form registered)
    virtual void store(Settings* s);                       // rows -> settings
    virtual bool command(int id) { return false; }         // a button of the page was pressed (its id from Form::button); true when handled
    virtual void state_changed() {}                        // the game folder or a running job changed: update the rows that depend on it

    Host* host() const { return host_; }
    // The row that follows `r` in the page (the second box of number_pair), or null.
    Row* row_after(const Row* r);

    // ---- the launcher's
    std::vector<std::unique_ptr<Row>>& rows() { return rows_; }
    void attach(Host* h) { host_ = h; }

private:
    friend class Form;
    Host* host_ = nullptr;
    std::vector<std::unique_ptr<Row>> rows_;
};

// What a game adds to the launcher (nxrt/game.h Info::launcher): tabs after the base's own and rows at the end of the Extras tab.
using PageFactory = std::unique_ptr<Page> (*)();
struct GameLauncher {
    const PageFactory* tabs = nullptr;
    unsigned tab_count = 0;
    void (*extras_rows)(Form& f) = nullptr;
};

}  // namespace nxl
