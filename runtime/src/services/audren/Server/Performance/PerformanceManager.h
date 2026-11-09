// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Performance/PerformanceManager.cs (Ryubing fork, MIT).
// Abstract base; the static factory and size query dispatch on the performance-metrics data format (1 or 2).
// Circular dependency: the base's statics construct PerformanceManagerGeneric, which derives from this class. The definitions of
// the static templates live in PerformanceManagerGeneric.h, which this header includes at the end (after the class is complete).
// Including either header on its own is enough.
//
// BehaviourInfo is not ported yet (it is in Server/BehaviourInfo.cs, separate work), so the two statics take it as a template
// parameter TBehaviourInfo. Any type with `uint32_t GetPerformanceMetricsDataFormat() const` works; when BehaviourInfo.h lands,
// callers pass it unchanged.
#pragma once
#include <cstdint>
#include <memory>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "PerformanceEntryAddresses.h"

namespace nxrt::audren {

class PerformanceManager {
  public:
    virtual ~PerformanceManager() = default;

    template <class TBehaviourInfo>
    static uint64_t GetRequiredBufferSizeForPerformanceMetricsPerFrame(AudioRendererConfiguration& parameter, const TBehaviourInfo& behaviourInfo);

    virtual uint32_t CopyHistories(Span<uint8_t> performanceOutput) = 0;

    virtual void SetTargetNodeId(int32_t target) = 0;

    virtual bool IsTargetNodeId(int32_t target) = 0;

    // Get the next buffer to store a performance entry. Returns true if a valid PerformanceEntryAddresses was returned.
    virtual bool GetNextEntry(PerformanceEntryAddresses& performanceEntry, PerformanceEntryType entryType, int32_t nodeId) = 0;

    // Get the next buffer to store a performance detailed entry. Returns true if a valid PerformanceEntryAddresses was returned.
    virtual bool GetNextEntry(PerformanceEntryAddresses& performanceEntry, PerformanceDetailType detailType, PerformanceEntryType entryType,
                              int32_t nodeId) = 0;

    virtual void TapFrame(bool dspRunningBehind, uint32_t voiceDropCount, uint64_t startRenderingTicks) = 0;

    template <class TBehaviourInfo>
    static std::unique_ptr<PerformanceManager> Create(Span<uint8_t> performanceBuffer, AudioRendererConfiguration& parameter,
                                                      const TBehaviourInfo& behaviourInfo);
};

}

#include "PerformanceManagerGeneric.h"
