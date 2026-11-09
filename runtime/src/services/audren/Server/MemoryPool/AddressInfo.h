// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/MemoryPool/AddressInfo.cs (Ryubing fork, MIT). A CPU region shared with the DSP,
// optionally backed by a MemoryPoolInfo. Field types are uint64_t for the same reason as in MemoryPoolInfo.h.
#pragma once
#include "MemoryPoolInfo.h"

namespace nxrt::audren {

struct AddressInfo {
    uint64_t CpuAddress = 0;
    uint64_t Size = 0;
    // The forced DSP address of the region (used when no pool backs it and force mapping is enabled).
    uint64_t ForceMappedDspAddress = 0;

  private:
    MemoryPoolInfo* _memoryPools = nullptr;

    // C# private property `ref *_memoryPools`. Renamed because a member called MemoryPoolInfo would hide the type name.
    MemoryPoolInfo& MemoryPoolInfoRef() const { return *_memoryPools; }

  public:
    bool HasMemoryPoolState() const { return _memoryPools != nullptr; }

    static AddressInfo Create() { return Create(0, 0); }

    static AddressInfo Create(uint64_t cpuAddress, uint64_t size) {
        AddressInfo info;
        info.CpuAddress = cpuAddress;
        info._memoryPools = MemoryPoolInfo::Null();
        info.Size = size;
        info.ForceMappedDspAddress = 0;
        return info;
    }

    void Setup(uint64_t cpuAddress, uint64_t size) {
        CpuAddress = cpuAddress;
        Size = size;
        ForceMappedDspAddress = 0;
        _memoryPools = MemoryPoolInfo::Null();
    }

    void SetupMemoryPool(Span<MemoryPoolInfo> memoryPoolState) {
        NXAUDREN_ASSERT(!memoryPoolState.IsEmpty());
        SetupMemoryPool(memoryPoolState.Data);
    }

    void SetupMemoryPool(MemoryPoolInfo* memoryPoolState) { _memoryPools = memoryPoolState; }

    bool HasMappedMemoryPool() const { return HasMemoryPoolState() && MemoryPoolInfoRef().IsMapped(); }

    uint64_t GetReference(bool markUsed) const {
        if (!HasMappedMemoryPool()) {
            return ForceMappedDspAddress;
        }

        if (markUsed) {
            MemoryPoolInfoRef().IsUsed = true;
        }

        return MemoryPoolInfoRef().Translate(CpuAddress, Size);
    }
};

}
