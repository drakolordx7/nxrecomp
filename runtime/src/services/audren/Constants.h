// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Constants.cs (Ryubing fork, MIT). The C# static class is kept as struct Constants with the same member names.
// Six of these values already exist at namespace scope in audren_types.h; they are forwarded here, not defined twice.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "audren_common.h"
#include "audren_types.h"

namespace nxrt::audren {

struct Constants {
    static constexpr const char* DefaultDeviceOutputName = "DeviceOut";
    static constexpr const char* DefaultDeviceInputName = "BuiltInHeadset";
    // The maximum number of channels supported. (6 channels for 5.1 surround)
    static constexpr int32_t ChannelCountMax = static_cast<int32_t>(::nxrt::audren::ChannelCountMax);
    static constexpr int32_t VoiceChannelCountMax = static_cast<int32_t>(::nxrt::audren::VoiceChannelCountMax);
    static constexpr int32_t MixBufferCountMax = static_cast<int32_t>(::nxrt::audren::MixBufferCountMax);
    static constexpr int32_t VoiceWaveBufferCount = static_cast<int32_t>(::nxrt::audren::VoiceWaveBufferCount);
    static constexpr int32_t VoiceBiquadFilterCount = static_cast<int32_t>(::nxrt::audren::VoiceBiquadFilterCount);
    static constexpr int32_t VoiceLowestPriority = 0xFF;
    static constexpr int32_t VoiceHighestPriority = 0;
    static constexpr int32_t MaxErrorInfos = static_cast<int32_t>(::nxrt::audren::MaxErrorInfos);
    static constexpr int32_t BufferAlignment = 0x40;
    static constexpr int32_t WorkBufferAlignment = 0x1000;
    static constexpr int32_t PerformanceMetricsPerFramesSizeAlignment = 0x100;
    static constexpr int32_t FinalMixId = 0;
    static constexpr int32_t UnusedMixId = 0x7FFFFFFF;
    static constexpr int32_t UnusedSplitterIdInt = -1;
    static constexpr uint32_t UnusedSplitterId = 0xFFFFFFFFu;
    static constexpr int32_t InvalidNodeId = -268435456;
    static constexpr int32_t InvalidProcessingOrder = -1;
    static constexpr int32_t AudioRendererSessionCountMax = 2;
    static constexpr int32_t AudioOutSessionCountMax = 12;
    static constexpr int32_t AudioInSessionCountMax = 4;
    static constexpr int32_t AudioDeviceBufferCountMax = 32;
    // The target sample rate of the audio renderer. (48kHz)
    static constexpr uint32_t TargetSampleRate = 48000;
    // The target sample size of the audio renderer. (PCM16)
    static constexpr int32_t TargetSampleSize = static_cast<int32_t>(sizeof(uint16_t));
    static constexpr int32_t TargetSampleCount = 240;
    static constexpr int32_t UpSampleEntrySize = TargetSampleCount * VoiceChannelCountMax;
    // The target audio latency computed from TargetSampleRate and TargetSampleCount. (5.00 ms)
    static constexpr int32_t AudioProcessorMaxUpdateTimeTarget =
        1000000000 / (static_cast<int32_t>(TargetSampleRate) / TargetSampleCount);
    // The maximum update time of the DSP on original hardware. (5.76 ms)
    static constexpr int32_t AudioProcessorMaxUpdateTime = 5760000;
    static constexpr int32_t AudioProcessorMaxUpdateTimePerSessions =
        AudioProcessorMaxUpdateTime / AudioRendererSessionCountMax;
    // Guest timer frequency used for system ticks.
    static constexpr int32_t TargetTimerFrequency = 19200000;
    // The default coefficients used for standard 5.1 surround to stereo downmixing.
    // C# static readonly float[]: allocated once, so a const std::vector.
    static inline const std::vector<float> DefaultSurroundToStereoCoefficients = {1.0f, 0.707f, 0.251f, 0.707f};
};

}
