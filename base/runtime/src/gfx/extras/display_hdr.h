// SPDX-License-Identifier: GPL-3.0-or-later
// Display HDR query (display_hdr.h): whether the monitor under the window is in an HDR mode, how bright SDR white is on it and how bright it
// can get. One implementation for every platform, on the SDL3 window and display properties (SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN,
// SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT). SDL fills those from DXGI and the Windows display
// configuration on Windows and from the compositor's colour management on Linux where it has one; where it has nothing the answer is
// "SDR". The platform layer (nxrt/platform.h) must be initialised. Never throws.
#pragma once

namespace nxrt::extras {

struct DisplayHdrInfo {
    bool valid = false;            // the query found the display and answered
    bool hdr_enabled = false;      // the display has HDR headroom above SDR white (Windows: advanced colour with the HDR10 colour space)
    float max_nits = 0;            // peak luminance (SDR white times the HDR headroom), 0 when unknown (SDR displays, and where SDL reports none)
    float sdr_white_nits = 80;     // the brightness of SDR white in nits (80 = the slider at minimum on Windows); 80 when unknown
    char monitor[64] = {};         // the display's name as SDL reports it
    char how[96] = {};             // a short note on what the answer was based on / what failed, for the log
};

// The display of the platform window; without a window the primary display (then only hdr_enabled is known). Returns out->valid. out must
// not be null (returns false if it is).
bool query_display_hdr(DisplayHdrInfo* out);

}  // namespace nxrt::extras
