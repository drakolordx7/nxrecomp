// SPDX-License-Identifier: GPL-3.0-or-later
// Display HDR query on SDL3 (display_hdr.h). Each call reads the properties fresh: SDL keeps them current (on Windows from DXGI and the display
// configuration database, which is what this file used to query itself) and sends SDL_EVENT_WINDOW_HDR_STATE_CHANGED when they change.
#include "extras/display_hdr.h"

#include <SDL3/SDL.h>

#include <cstdio>

#include "nxrt/platform.h"

namespace nxrt::extras {
namespace {

// SDL_COLORSPACE_SRGB_LINEAR: 1.0 is 80 nits.
constexpr float kNitsPerSdrUnit = 80.0f;

template <size_t N>
void put(char (&dst)[N], const char* text) {
    std::snprintf(dst, N, "%s", text ? text : "");
}

void run(DisplayHdrInfo* out) {
    if (!platform::initialised()) {
        put(out->how, "platform not initialised");
        return;
    }
    SDL_Window* win = static_cast<SDL_Window*>(platform::native_window());
    const SDL_DisplayID display = win ? SDL_GetDisplayForWindow(win) : SDL_GetPrimaryDisplay();
    if (!display) {
        std::snprintf(out->how, sizeof out->how, "SDL: no display (%s)", SDL_GetError());
        return;
    }
    put(out->monitor, SDL_GetDisplayName(display));

    if (win) {
        const SDL_PropertiesID p = SDL_GetWindowProperties(win);
        const float white = SDL_GetFloatProperty(p, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
        const float headroom = SDL_GetFloatProperty(p, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
        out->hdr_enabled = SDL_GetBooleanProperty(p, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
        out->sdr_white_nits = (white >= 1.0f ? white : 1.0f) * kNitsPerSdrUnit;
        if (out->hdr_enabled) out->max_nits = out->sdr_white_nits * (headroom >= 1.0f ? headroom : 1.0f);
        std::snprintf(out->how, sizeof out->how, "SDL window: hdr %d, SDR white x%.2f, headroom x%.2f", out->hdr_enabled ? 1 : 0, white, headroom);
    } else {
        out->hdr_enabled = SDL_GetBooleanProperty(SDL_GetDisplayProperties(display), SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
        std::snprintf(out->how, sizeof out->how, "SDL display: hdr %d (no window: no levels)", out->hdr_enabled ? 1 : 0);
    }
    out->valid = true;
}

}  // namespace

bool query_display_hdr(DisplayHdrInfo* out) {
    if (!out) return false;
    *out = DisplayHdrInfo{};
    run(out);
    return out->valid;
}

}  // namespace nxrt::extras
