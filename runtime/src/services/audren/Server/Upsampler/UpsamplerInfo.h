// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Upsampler/UpsamplerInfo.cs (Ryubing fork, MIT). One allocated upsampler slot.
// UpsamplerBufferState is reused from Dsp/UpsamplerHelper.h.
//
// Release() is defined at the end of UpsamplerManager.h, because it needs the complete UpsamplerManager type.
#pragma once
#include <cstdint>
#include <vector>

#include "../../Dsp/UpsamplerHelper.h"
#include "../../audren_common.h"

namespace nxrt::audren {

class UpsamplerManager;

class UpsamplerInfo {
  public:
    Span<float> OutputBuffer;
    uint32_t SampleCount = 0;
    uint32_t SourceSampleCount = 0;
    std::vector<uint16_t> InputBufferIndices;
    std::vector<UpsamplerBufferState> BufferStates;

  private:
    int32_t _index;
    UpsamplerManager* _manager;

  public:
    UpsamplerInfo(UpsamplerManager& manager, int32_t index, Span<float> outputBuffer, uint32_t sampleCount)
        : OutputBuffer(outputBuffer), SampleCount(sampleCount), _index(index), _manager(&manager) {}

    // Release the UpsamplerInfo. The manager destroys this object, so nothing may touch it after this call.
    void Release();
};

}
