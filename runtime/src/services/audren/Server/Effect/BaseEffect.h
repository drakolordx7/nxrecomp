// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Effect/BaseEffect.cs (Ryubing fork, MIT).
//
// Naming notes:
//   * C# `UsageState` (a member of the same name as its type) is written `::nxrt::audren::UsageState` in member code,
//     because the member name hides the enum type inside the class.
//   * C# `TargetEffectType` (a property with a body) is the method TargetEffectType() const.
//   * C# `ref BaseEffect` slots are held by EffectContext as std::unique_ptr<BaseEffect>, so derived classes replace the object.
//   * C# `IEffectInParameter` and `IEffectOutStatus` are the concepts in EffectParameterHelpers.h.
#pragma once
#include <cstdint>
#include <limits>
#include <vector>

#include "../../Constants.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/AddressInfo.h"
#include "../MemoryPool/PoolMapper.h"
#include "EffectParameterHelpers.h"
#include "UsageState.h"

namespace nxrt::audren {

class BaseEffect {
  public:
    // The EffectType of the effect. The base class leaves it Invalid; derived constructors set it from TargetEffectType()
    // (a virtual call is not dispatched from a base constructor in C++).
    EffectType Type = EffectType::Invalid;
    bool IsEnabled = false;
    bool BufferUnmapped = false;
    ::nxrt::audren::UsageState UsageState = ::nxrt::audren::UsageState::Invalid;
    int32_t MixId = Constants::UnusedMixId;
    uint32_t ProcessingOrder = std::numeric_limits<uint32_t>::max();

  protected:
    std::vector<AddressInfo> WorkBuffers;

  public:
    BaseEffect() : WorkBuffers(2, AddressInfo::Create()) {}
    BaseEffect(const BaseEffect&) = delete;
    BaseEffect& operator=(const BaseEffect&) = delete;
    virtual ~BaseEffect() = default;

    virtual EffectType TargetEffectType() const { return EffectType::Invalid; }

    template <IEffectInParameter T>
    bool IsTypeValid(const T& parameter) const {
        return parameter.type == TargetEffectType();
    }

    void ForceUnmapBuffers(PoolMapper& mapper) {
        for (AddressInfo& info : WorkBuffers) {
            if (info.GetReference(false) != 0) {
                mapper.ForceUnmap(info);
            }
        }
    }

    bool ShouldSkip() const { return BufferUnmapped; }

    virtual void UpdateForCommandGeneration() { NXAUDREN_ASSERT(Type == TargetEffectType()); }

    virtual void InitializeResultState([[maybe_unused]] EffectResultState& state) {}

    virtual void UpdateResultState([[maybe_unused]] EffectResultState& destState, [[maybe_unused]] EffectResultState& srcState) {}

    virtual void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, [[maybe_unused]] const EffectInParameterVersion1& parameter,
                        [[maybe_unused]] PoolMapper& mapper) {
        NXAUDREN_ASSERT(IsTypeValid(parameter));

        updateErrorInfo = BehaviourParameter::ErrorInfo{};
    }

    virtual void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, [[maybe_unused]] const EffectInParameterVersion2& parameter,
                        [[maybe_unused]] PoolMapper& mapper) {
        NXAUDREN_ASSERT(IsTypeValid(parameter));

        updateErrorInfo = BehaviourParameter::ErrorInfo{};
    }

    virtual DspAddress GetWorkBuffer([[maybe_unused]] int index) { NXAUDREN_FATAL("GetWorkBuffer is not implemented for this effect"); }

    template <IEffectOutStatus T>
    void StoreStatus(T& outStatus, bool isAudioRendererActive) const {
        if (isAudioRendererActive) {
            if (UsageState == ::nxrt::audren::UsageState::Disabled) {
                outStatus.state = EffectState::Disabled;
            } else {
                outStatus.state = EffectState::Enabled;
            }
        } else if (UsageState == ::nxrt::audren::UsageState::New) {
            outStatus.state = EffectState::Enabled;
        } else {
            outStatus.state = EffectState::Disabled;
        }
    }

    // The PerformanceDetailType associated to the Type of this effect (C# throws NotImplementedException for Invalid).
    PerformanceDetailType GetPerformanceDetailType() const {
        switch (Type) {
            case EffectType::BiquadFilter:
                return PerformanceDetailType::BiquadFilter;
            case EffectType::AuxiliaryBuffer:
                return PerformanceDetailType::Aux;
            case EffectType::Delay:
                return PerformanceDetailType::Delay;
            case EffectType::Reverb:
                return PerformanceDetailType::Reverb;
            case EffectType::Reverb3d:
                return PerformanceDetailType::Reverb3d;
            case EffectType::BufferMix:
                return PerformanceDetailType::Mix;
            case EffectType::Limiter:
                return PerformanceDetailType::Limiter;
            case EffectType::CaptureBuffer:
                return PerformanceDetailType::CaptureBuffer;
            case EffectType::Compressor:
                return PerformanceDetailType::Compressor;
            default:
                NXAUDREN_FATAL("EffectType has no PerformanceDetailType");
        }
    }

  protected:
    void UpdateUsageStateForCommandGeneration() {
        UsageState = IsEnabled ? ::nxrt::audren::UsageState::Enabled : ::nxrt::audren::UsageState::Disabled;
    }

    template <IEffectInParameter T>
    void UpdateParameterBase(const T& parameter) {
        MixId = parameter.mix_id;
        ProcessingOrder = parameter.processing_order;
    }

    // C# GetSingleBuffer: the first work buffer, or 0 when the effect is disabled.
    DspAddress GetSingleBuffer() {
        if (IsEnabled) {
            return WorkBuffers[0].GetReference(true);
        }

        if (UsageState != ::nxrt::audren::UsageState::Disabled) {
            DspAddress address = WorkBuffers[0].GetReference(false);
            uint64_t size = WorkBuffers[0].Size;

            if (address != 0 && size != 0) {
                EffectParameterHelpers::InvalidateDataCache(address, size);
            }
        }

        return 0;
    }
};

}
