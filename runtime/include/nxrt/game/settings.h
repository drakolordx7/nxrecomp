// SPDX-License-Identifier: GPL-3.0-or-later
// A game's own section of the play-mode settings file (controls.ini).
//
// The base owns the sections every game has ([mouse] [pad] [display] ...). A game that has settings of its own (optional looks, start-up
// shortcuts, anything its hooks read) registers them with nxrt/game.h Info::settings:
//   * a table of options (GameOption): a key in a section of the base or in a section of the game's own, with its range and its row on the F1
//     page and in the launcher; the base reads, writes and shows them;
//   * the section name in controls.ini and the reader and writer of its other keys (the game and the launcher read and write the file with the
//     same code, so this part is in the game's data library, which the launcher links);
//   * its page of the F1 overlay: a tab of its own and rows at the end of the Extras tab (Dear ImGui; the game's play library);
//   * its launcher pages: nxrt/game/launcher_ui.h, Info::launcher.
// The values live in Settings::game, a copyable box for whatever type the game keeps there. The base never looks inside it.
//
// Without a registered section the file has no such section, the F1 overlay has no such tab and the launcher no such page.
#pragma once
#include <memory>
#include <string>
#include <typeinfo>

namespace nxrt::play {

struct Settings;

// One value of any copyable, default-constructible type, copied with the Settings that hold it. get<T>() always uses the same T for one
// game; a const get on an empty box returns a default T.
class GameState {
public:
    GameState() = default;
    GameState(const GameState& o) : p_(o.p_ ? o.p_->clone() : nullptr) {}
    GameState& operator=(const GameState& o) {
        if (this != &o) p_ = o.p_ ? o.p_->clone() : nullptr;
        return *this;
    }
    GameState(GameState&&) = default;
    GameState& operator=(GameState&&) = default;

    template <class T>
    T& get() {
        if (!p_ || p_->type() != typeid(T)) p_ = std::make_unique<Holder<T>>();
        return static_cast<Holder<T>&>(*p_).v;
    }
    template <class T>
    const T& get() const {
        static const T none{};
        return p_ && p_->type() == typeid(T) ? static_cast<const Holder<T>&>(*p_).v : none;
    }

private:
    struct Base {
        virtual ~Base() = default;
        virtual std::unique_ptr<Base> clone() const = 0;
        virtual const std::type_info& type() const = 0;
    };
    template <class T>
    struct Holder : Base {
        T v{};
        std::unique_ptr<Base> clone() const override { return std::make_unique<Holder<T>>(*this); }
        const std::type_info& type() const override { return typeid(T); }
    };
    std::unique_ptr<Base> p_;
};

// One option a game adds to a section the base owns ([mouse] [pad] [display]) or to a section of its own ([aim]). A row drives the reader, the
// writer and the rows of the F1 page and the launcher: the game lists what the option is and the base shows it. The value is a double
// whatever the kind (Bool 0/1, Choice the index into `words`), kept by the game's get and set in its own state (Settings::game).
enum class OptKind : uint8_t { Bool, Choice, Real };
enum class OptTab : uint8_t { Mouse, Pad, Display };      // the F1 tab and the launcher page the row sits on (Mouse and Pad: Controls)
constexpr uint8_t kOptF1 = 1, kOptLauncher = 2;           // GameOption::ui: where the row is shown (0: the file only)

struct GameOption {
    const char* section;                  // controls.ini section: "display", "mouse", "pad" or a section the game owns
    const char* key;                      // the key as the file has it
    const char* alias = nullptr;          // a spelling that is read as well
    OptKind kind = OptKind::Bool;
    double lo = 0, hi = 1;                // Real: the range the file accepts
    const char* const* words = nullptr;   // Choice: the file's words, in value order
    unsigned word_count = 0;
    double (*get)(const GameState&) = nullptr;
    void (*set)(GameState&, double) = nullptr;
    bool write = true;                    // false: the key is read but never written (another row writes the value)
    bool (*emit)(const GameState&) = nullptr;   // null: always written; otherwise written when it returns true
    // The row.
    uint8_t ui = 0;
    OptTab tab = OptTab::Mouse;
    const char* group = nullptr;          // a heading above this row and the rows after it (launcher) / a separator (F1); null: no new heading
    const char* label = "";               // UTF-8
    const char* hint = nullptr;           // grey text under the row
    const char* const* labels = nullptr;  // Choice: the text of each word
    bool inverted = false;                // Bool: shown as "not value"
    const char* depends = nullptr;        // the key of a Bool or Choice row of the same game: this row is enabled while that value is not 0
    double ui_lo = 0, ui_hi = 0;          // Real: the range the slider shows (units of the shown value)
    double ui_scale = 1;                  // shown value = stored value * ui_scale
    int ui_decimals = 0;                  // digits after the point of the shown value (a launcher slider steps by 10^-decimals)
    const char* ui_unit = "";             // shown after the value
    bool ui_log = false;                  // F1: a logarithmic slider
};

struct GameSection {
    const char* name = "";     // the controls.ini section the game owns, without brackets: "tweaks"
    // One `key = value` of the section. Returns false with *msg set for a bad value of a key the game has; *known = false (return true) for
    // a key it does not have, so the base reports it as unknown.
    bool (*apply)(GameState& state, const std::string& key, const std::string& value, bool* known, std::string* msg) = nullptr;
    // Appends the section's "key = value" lines (no header).
    void (*format)(const GameState& state, std::string* out) = nullptr;
    // The options the game adds (above). The data is in the game's data library, which the launcher links.
    const GameOption* options = nullptr;
    unsigned option_count = 0;
    const char* mouse_note = nullptr;               // text under the F1 Mouse tab (UTF-8)
    // F1 overlay (Dear ImGui, play mode only; null in a section the launcher links). Each returns true when the user changed `edit`.
    const char* tab_title = nullptr;                // a tab of its own after the base's tabs (null: none)
    bool (*tab)(Settings& edit) = nullptr;
    bool (*extras_rows)(Settings& edit) = nullptr;  // rows at the end of the Extras tab
};

const GameSection* game_section();    // the registered game's (nxrt/game.h Info::settings), or nullptr

}  // namespace nxrt::play
