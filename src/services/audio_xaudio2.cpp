// SPDX-License-Identifier: GPL-3.0-or-later
// XAudio2 host sink: placeholder until the sink is written (S3c, audren).
#include "nxrt/audio_sink.h"

namespace nxrt::audio {
std::shared_ptr<AudioSink> make_xaudio2_sink() { return nullptr; }
}  // namespace nxrt::audio
