// SPDX-License-Identifier: GPL-3.0-or-later
// A game's own section of the play-mode settings file (controls.ini).
//
// The base owns the sections every game has ([mouse] [pad] [display] ...). A game that has settings of its own (optional looks, start-up
// shortcuts, anything its hooks read) registers one more section with nxrt/game.h Info::settings:
//   * the section name in controls.ini and the reader and writer of its keys (the game and the launcher read and write the file with the same
//     code, so this part is in the game's data library, which the launcher links);
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

struct GameSection {
    const char* name = "";     // the controls.ini section the game owns, without brackets: "tweaks"
    // One `key = value` of the section. Returns false with *msg set for a bad value of a key the game has; *known = false (return true) for
    // a key it does not have, so the base reports it as unknown.
    bool (*apply)(GameState& state, const std::string& key, const std::string& value, bool* known, std::string* msg) = nullptr;
    // Appends the section's "key = value\n" lines (no header).
    void (*format)(const GameState& state, std::string* out) = nullptr;
    // F1 overlay (Dear ImGui, play mode only; null in a section the launcher links). Each returns true when the user changed `edit`.
    const char* tab_title = nullptr;                // a tab of its own after the base's tabs (null: none)
    bool (*tab)(Settings& edit) = nullptr;
    bool (*extras_rows)(Settings& edit) = nullptr;  // rows at the end of the Extras tab
};

const GameSection* game_section();    // the registered game's (nxrt/game.h Info::settings), or nullptr

}  // namespace nxrt::play
