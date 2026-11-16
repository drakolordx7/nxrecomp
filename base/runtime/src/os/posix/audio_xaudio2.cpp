// SPDX-License-Identifier: GPL-3.0-or-later
// XAudio2 is Windows only (src/os/win32/audio_xaudio2.cpp): here the factory says so and the SDL sink is used.
#include "nxrt/audio_sink.h"

namespace nxrt::audio {

std::shared_ptr<AudioSink> make_xaudio2_sink() { return nullptr; }

}  // namespace nxrt::audio
