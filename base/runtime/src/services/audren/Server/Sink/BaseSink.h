// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Sink/BaseSink.cs (Ryubing fork, MIT). Base server state of an audio sink.
//
// C# calls the virtual CleanUp() from this constructor, and C# dispatches it to the derived override even during base
// construction. C++ does not, so each derived constructor calls CleanUp() again once its own part is built, which gives the
// same final state.
#pragma once
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/PoolMapper.h"

namespace nxrt::audren {

class BaseSink {
  public:
    SinkType Type = SinkType::Invalid;
    bool IsUsed = false;
    bool ShouldSkip = false;
    int32_t NodeId = 0;

    BaseSink() { CleanUp(); }
    virtual ~BaseSink() = default;

    virtual void CleanUp() {
        Type = TargetSinkType();
        IsUsed = false;
        ShouldSkip = false;
    }

    virtual SinkType TargetSinkType() const { return SinkType::Invalid; }

    bool IsTypeValid(const SinkInParameter& parameter) const { return parameter.type == TargetSinkType(); }

    virtual void UpdateForCommandGeneration() { NXAUDREN_ASSERT(Type == TargetSinkType()); }

  protected:
    void UpdateStandardParameter(const SinkInParameter& parameter) {
        if (IsUsed != parameter.is_used) {
            IsUsed = parameter.is_used;
            NodeId = parameter.node_id;
        }
    }

  public:
    virtual void Update(BehaviourParameter::ErrorInfo& errorInfo, const SinkInParameter& parameter, SinkOutStatus& outStatus,
                        PoolMapper& mapper) {
        NXAUDREN_ASSERT(IsTypeValid(parameter));
        (void)parameter;
        (void)outStatus;
        (void)mapper;

        errorInfo = BehaviourParameter::ErrorInfo{};
    }
};

}
