// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Performance/PerformanceFrameHeaderVersion1.cs (Ryubing fork, MIT).
// C# [StructLayout(Sequential, Pack = 1, Size = 0x18)]: the fields fill the whole 0x18 bytes, no padding.
#pragma once
#include "IPerformanceHeader.h"

namespace nxrt::audren {

#pragma pack(push, 1)
struct PerformanceFrameHeaderVersion1 {
    uint32_t Magic;
    int32_t EntryCount;
    int32_t EntryDetailCount;
    int32_t NextOffset;
    int32_t TotalProcessingTime;
    uint32_t VoiceDropCount;

    int32_t GetEntryCount() const { return EntryCount; }
    int32_t GetEntryCountOffset() const { return 4; }
    int32_t GetEntryDetailCount() const { return EntryDetailCount; }
    void SetDspRunningBehind(bool) {
        // NOTE: Not present in version 1
    }
    void SetEntryCount(int32_t entryCount) { EntryCount = entryCount; }
    void SetEntryDetailCount(int32_t entryDetailCount) { EntryDetailCount = entryDetailCount; }
    void SetIndex(uint32_t) {
        // NOTE: Not present in version 1
    }
    void SetMagic(uint32_t magic) { Magic = magic; }
    void SetNextOffset(int32_t nextOffset) { NextOffset = nextOffset; }
    void SetStartRenderingTicks(uint64_t) {
        // NOTE: not present in version 1
    }
    void SetTotalProcessingTime(int32_t totalProcessingTime) { TotalProcessingTime = totalProcessingTime; }
    void SetVoiceDropCount(uint32_t voiceCount) { VoiceDropCount = voiceCount; }
};
#pragma pack(pop)

static_assert(sizeof(PerformanceFrameHeaderVersion1) == 0x18);
static_assert(IPerformanceHeader<PerformanceFrameHeaderVersion1>);

}
