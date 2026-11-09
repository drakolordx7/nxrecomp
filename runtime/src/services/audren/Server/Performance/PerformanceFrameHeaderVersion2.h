// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Performance/PerformanceFrameHeaderVersion2.cs (Ryubing fork, MIT).
// C# [StructLayout(Sequential, Pack = 1, Size = 0x30)]: the fields take 37 bytes, the declared size adds 11 bytes of trailing padding.
#pragma once
#include "IPerformanceHeader.h"

namespace nxrt::audren {

#pragma pack(push, 1)
struct PerformanceFrameHeaderVersion2 {
    uint32_t Magic;
    int32_t EntryCount;
    int32_t EntryDetailCount;
    int32_t NextOffset;
    int32_t TotalProcessingTime;
    uint32_t VoiceDropCount;
    uint64_t StartRenderingTicks;
    uint32_t Index;
    bool IsDspRunningBehind;
    // Trailing padding up to the C# Size (0x30).
    uint8_t Padding[11];

    int32_t GetEntryCount() const { return EntryCount; }
    int32_t GetEntryCountOffset() const { return 4; }
    int32_t GetEntryDetailCount() const { return EntryDetailCount; }
    void SetDspRunningBehind(bool isRunningBehind) { IsDspRunningBehind = isRunningBehind; }
    void SetEntryCount(int32_t entryCount) { EntryCount = entryCount; }
    void SetEntryDetailCount(int32_t entryDetailCount) { EntryDetailCount = entryDetailCount; }
    void SetIndex(uint32_t index) { Index = index; }
    void SetMagic(uint32_t magic) { Magic = magic; }
    void SetNextOffset(int32_t nextOffset) { NextOffset = nextOffset; }
    void SetStartRenderingTicks(uint64_t startTicks) { StartRenderingTicks = startTicks; }
    void SetTotalProcessingTime(int32_t totalProcessingTime) { TotalProcessingTime = totalProcessingTime; }
    void SetVoiceDropCount(uint32_t voiceCount) { VoiceDropCount = voiceCount; }
};
#pragma pack(pop)

static_assert(sizeof(PerformanceFrameHeaderVersion2) == 0x30);
static_assert(IPerformanceHeader<PerformanceFrameHeaderVersion2>);

}
