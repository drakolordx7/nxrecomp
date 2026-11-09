// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Voice/VoiceChannelResource.cs (Ryubing fork, MIT). Server state of one voice channel.
// The C# struct is Size 0xD0 with Pack = 0x10; the C++ struct is alignas(16) so that sizeof matches that size.
#pragma once
#include <array>
#include <cstdint>

#include "../../audren_types.h"

namespace nxrt::audren {

struct alignas(0x10) VoiceChannelResource {
    static constexpr uint32_t Alignment = 0x10;

    std::array<float, MixBufferCountMax> Mix{};

    std::array<float, MixBufferCountMax> PreviousMix{};

    uint32_t Id = 0;

    bool IsUsed = false;

    void UpdateState() { PreviousMix = Mix; }
};

static_assert(sizeof(VoiceChannelResource) == 0xD0);

}
