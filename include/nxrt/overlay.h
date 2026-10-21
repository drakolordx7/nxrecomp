// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui overlay on the Vulkan path (stream S4b): the settings / developer menu of the PC port. Hidden by default; a key
// (F1, [gfx] overlay_key = any SDL scancode name) toggles it. ImGui is fed from the nxrt input queue (input.h), not from an
// ImGui platform backend, so it works the same with a window and headless. While it is visible and wants the mouse or the
// keyboard, handle_event() returns true and the game must not see the event. Showing the overlay releases the relative mouse
// mode and hiding it restores it.
// Changes made in the menu that touch the swapchain or the window (present mode, fullscreen) are applied by apply_pending(),
// which the frame loop calls before Renderer::begin_frame().
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "nxrt/frame_limiter.h"
#include "nxrt/gfx.h"
#include "nxrt/input.h"

namespace nxrt::gfx {

class Overlay {
public:
    struct Context {
        Device* device = nullptr;
        Renderer* renderer = nullptr;
        FrameLimiter* limiter = nullptr;     // optional: shows and sets the frame cap
        bool has_window = true;              // false: no window controls (headless)
    };
    static std::unique_ptr<Overlay> create(const Context& ctx, std::string* error = nullptr);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Feed every input event here (before the game sees it). Returns true when the overlay consumed it.
    bool handle_event(const input::Event& e);

    void set_visible(bool v);
    bool visible() const;
    void toggle();
    int toggle_scancode() const;             // SDL scancode of the toggle key
    void set_toggle_scancode(int sc);
    bool wants_input() const;                // visible and ImGui wants the mouse or keyboard

    // Extra panels for later streams (settings, dev menu): fn runs inside an ImGui window titled `title` while visible.
    void add_panel(const std::string& title, std::function<void()> fn);

    // Applies menu choices that cannot be made in the middle of a frame. Call before Renderer::begin_frame().
    void apply_pending();

    // The Renderer overlay hook: builds and draws the menu on top of the frame (no-op while hidden).
    void draw(const FrameInfo& f);
    void install_on_renderer();              // renderer->set_overlay_hook(draw)

    uint64_t frames_drawn() const;

private:
    Overlay() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace nxrt::gfx
