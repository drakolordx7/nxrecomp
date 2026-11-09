// SPDX-License-Identifier: GPL-3.0-or-later
// Everything here is called from one thread (the one that created the window, normally the main thread). Events are not
// returned: pump_events() turns them into nxrt::input events (input.h).
// Configuration (config.h): [gfx] hidden / NXRT_GFX_HIDDEN=1 creates the window hidden (automated tests).
#pragma once
#include <cstdint>

namespace nxrt::platform {

struct InitOptions {
    bool gamepads = true;
};
bool init(const InitOptions& opt = {});   // idempotent; sets the SDL hints (raw mouse), SDL_Init(VIDEO|EVENTS|JOYSTICK|GAMEPAD)
void shutdown();
bool initialised();

struct WindowDesc {
    const char* title = "nxrt";
    int width = 1280, height = 720;     // client size in screen coordinates at creation
    bool resizable = true;
    bool hidden = false;                // also forced by [gfx] hidden / NXRT_GFX_HIDDEN
};
bool create_window(const WindowDesc& desc);   // Vulkan-capable window (SDL_WINDOW_VULKAN, high pixel density)
void destroy_window();
bool has_window();
void* native_window();                        // SDL_Window*
void* native_hwnd();
void set_window_title(const char* title);
void pixel_size(int* w, int* h);              // client size in pixels (0, 0 while minimised)
bool set_window_size(int w, int h);
bool minimized();
bool focused();
bool visible();

enum class FullscreenMode { Windowed, Borderless, Exclusive };
// Borderless: desktop-resolution borderless window. Exclusive: SDL fullscreen with a real display mode closest to w x h at hz
// (zeros: the desktop mode). Returns false when the change failed (the mode is then unchanged). Synchronous: waits until SDL
// reports the new state or ~2 s.
bool set_fullscreen(FullscreenMode mode, int w = 0, int h = 0, float hz = 0.f);
FullscreenMode fullscreen_mode();
float display_refresh_hz();

// Relative mouse mode: raw, unaccelerated deltas (SDL_SetWindowRelativeMouseMode with SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE
// forced to "0" and SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE to "1"). The cursor is hidden and confined while it is on.
bool set_relative_mouse(bool on);
bool relative_mouse();
void set_cursor_visible(bool on);             // the OS cursor over the window (not in relative mode, which hides it); no-op without a window
bool relative_mouse_is_raw();                 // true when the system-scale hint is off (the unaccelerated setup)

// Test aid ([test] mouse_sdl_hz): pushes one SDL_EVENT_MOUSE_MOTION with these relative counts into SDL's own event queue (any thread).
// pump_events() treats such an event as relative motion even when the window has no focus (a scripted run), so it reaches the aim the way a
// real mouse's events do. False when SDL refused the event (queue full). test_motion_received(): counts of those events pumped so far.
bool push_test_motion(float dx, float dy);
void test_motion_received(double* dx, double* dy, uint64_t* events);

// Polled gamepad state (play mode): what SDL's gamepad layer reports for one open pad. SDL normalises every controller to the Xbox
// layout by position (SOUTH is the bottom face button), whatever is printed on it.
struct GamepadRaw {
    uint32_t buttons = 0;               // bit n set = SDL_GamepadButton n is down (SOUTH 0, EAST 1, WEST 2, NORTH 3, BACK 4, GUIDE 5, START 6,
                                        // LEFT_STICK 7, RIGHT_STICK 8, LEFT_SHOULDER 9, RIGHT_SHOULDER 10, DPAD_UP 11, DOWN 12, LEFT 13, RIGHT 14, MISC1 15)
    int16_t axes[6] = {};               // SDL_GamepadAxis order: LEFTX LEFTY RIGHTX RIGHTY (-32768..32767, y down = +), LEFT_TRIGGER RIGHT_TRIGGER (0..32767)
    uint8_t face_label[4] = {0, 1, 2, 3};   // what is printed on SOUTH, EAST, WEST, NORTH: 0 = A, 1 = B, 2 = X, 3 = Y
                                        // (PlayStation cross, circle, square, triangle count as A, B, X, Y; a Nintendo pad prints B, A, Y, X)
};
bool gamepad_poll(uint32_t id, GamepadRaw* out);
bool gamepad_sensors(uint32_t id, float accel[3], float gyro[3]);      // latest accelerometer (m/s^2) and gyro (rad/s) in SDL's axes; false without both sensors
bool gamepad_rumble(uint32_t id, uint16_t low, uint16_t high, uint32_t duration_ms);   // 0..65535 per motor; (0, 0) stops it; false when unsupported

// Pumps the SDL event queue and translates it into input events and window state. wait: block for up to timeout_ms (-1 =
// forever) for the first event (used while minimised). Returns the number of SDL events handled.
int pump_events(bool wait = false, int timeout_ms = -1);
bool quit_requested();
void clear_quit();
void request_quit();

uint64_t ticks_ns();

// Vulkan: the instance extensions SDL needs (VK_KHR_surface + the platform one) and surface creation/destruction. The Vulkan
// handles are passed as integers so that this header needs no Vulkan include.
const char* const* vulkan_instance_extensions(uint32_t* count);
bool create_vulkan_surface(void* vk_instance, uint64_t* surface_out);
void destroy_vulkan_surface(void* vk_instance, uint64_t surface);

}
