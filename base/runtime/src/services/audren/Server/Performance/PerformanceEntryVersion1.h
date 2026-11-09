// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Performance/PerformanceEntryVersion1.cs (Ryubing fork, MIT).
// C# [StructLayout(Sequential, Pack = 1, Size = 0x10)]: the fields take 13 bytes, the declared size adds 3 bytes of trailing padding.
#pragma once
#include "IPerformanceEntry.h"

namespace nxrt::audren {

#pragma pack(push, 1)
struct PerformanceEntryVersion1 {
    int32_t NodeId;
    int32_t StartTime;
    int32_t ProcessingTime;
    PerformanceEntryType EntryType;
    // Trailing padding up to the C# Size (0x10).
    uint8_t Padding[3];

    int32_t GetProcessingTime() const { return ProcessingTime; }
    int32_t GetProcessingTimeOffset() const { return 8; }
    int32_t GetStartTime() const { return StartTime; }
    int32_t GetStartTimeOffset() const { return 4; }
    void SetEntryType(PerformanceEntryType type) { EntryType = type; }
    void SetNodeId(int32_t nodeId) { NodeId = nodeId; }
};
#pragma pack(pop)

static_assert(sizeof(PerformanceEntryVersion1) == 0x10);
static_assert(IPerformanceEntry<PerformanceEntryVersion1>);

}
