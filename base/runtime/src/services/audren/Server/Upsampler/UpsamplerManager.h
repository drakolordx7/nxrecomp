// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Upsampler/UpsamplerManager.cs (Ryubing fork, MIT). A fixed pool of upsampler slots,
// each backed by one TargetSampleCount-sized region of the upsampler work buffer.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "../../Constants.h"
#include "../../audren_common.h"
#include "UpsamplerInfo.h"

namespace nxrt::audren {

class UpsamplerManager {
  private:
    Span<float> _upSamplerWorkBuffer;
    std::mutex _lock;
    // The upsampler instances. A null slot is free. Owned here: Release() on an UpsamplerInfo frees its slot.
    std::vector<std::unique_ptr<UpsamplerInfo>> _upsamplers;
    uint32_t _count;

  public:
    UpsamplerManager(Span<float> upSamplerWorkBuffer, uint32_t count) : _upSamplerWorkBuffer(upSamplerWorkBuffer), _count(count) {
        _upsamplers.resize(_count);
    }

    // Allocate a new UpsamplerInfo. Returns nullptr when all slots are in use (C# returns null).
    UpsamplerInfo* Allocate() {
        int32_t workBufferOffset = 0;

        std::lock_guard<std::mutex> guard(_lock);
        for (uint32_t i = 0; i < _count; i++) {
            if (_upsamplers[i] == nullptr) {
                _upsamplers[i] = std::make_unique<UpsamplerInfo>(
                    *this, static_cast<int32_t>(i),
                    _upSamplerWorkBuffer.Slice(static_cast<size_t>(workBufferOffset), static_cast<size_t>(Constants::UpSampleEntrySize)),
                    static_cast<uint32_t>(Constants::TargetSampleCount));
                return _upsamplers[i].get();
            }

            workBufferOffset += Constants::UpSampleEntrySize;
        }

        return nullptr;
    }

    void Free(int32_t index) {
        std::lock_guard<std::mutex> guard(_lock);
        NXAUDREN_ASSERT(_upsamplers[static_cast<size_t>(index)] != nullptr);

        _upsamplers[static_cast<size_t>(index)].reset();
    }
};

// Defined here, not in UpsamplerInfo.h, because it needs the complete UpsamplerManager.
inline void UpsamplerInfo::Release() { _manager->Free(_index); }

}
