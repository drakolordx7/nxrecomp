// SPDX-License-Identifier: GPL-3.0-or-later
// Media Foundation does not exist here: the movie:: library plays through FFmpeg only (src/media/movie_ffmpeg.cpp). The Windows twin is
// src/os/win32/movie_mf.cpp.
#include "nxrt/movie_source.h"

namespace nxrt::media {

std::unique_ptr<MovieSource> make_mf_source() { return nullptr; }

}
