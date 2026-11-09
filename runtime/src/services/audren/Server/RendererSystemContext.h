// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/RendererSystemContext.cs (Ryubing fork, MIT).
// A lite version of AudioRenderSystem used by the audio processor, so that the DSP side does not depend on the whole system.
// Deviation: BehaviourInfo and UpsamplerManager are C# class instances owned by the AudioRenderSystem, so they are pointers here (not
// value members). A value copy would not see later updates (for example the user revision) made by the owner.
#pragma once
#include <cstdint>

#include "../audren_common.h"
#include "BehaviourInfo.h"
#include "Upsampler/UpsamplerManager.h"

namespace nxrt::audren {

class RendererSystemContext {
  public:
    int32_t SessionId = 0;

    uint32_t ChannelCount = 0;

    uint32_t MixBufferCount = 0;

    nxrt::audren::BehaviourInfo* BehaviourInfo = nullptr;

    nxrt::audren::UpsamplerManager* UpsamplerManager = nullptr;

    Span<float> DepopBuffer;
};

}
