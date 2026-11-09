// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/MemoryPool/MemoryPoolInfo.cs (Ryubing fork, MIT). Server state of one memory pool.
// This is not a binary layout (the guest sees MemoryPoolInParameter / MemoryPoolOutStatus), so it is a plain struct with the C# names.
// Field types are spelled uint64_t rather than the CpuAddress / DspAddress aliases: inside a struct whose data members have
// those names, the alias would be hidden by the member.
#pragma once
#include "../../audren_common.h"

namespace nxrt::audren {

struct MemoryPoolInfo {
    static constexpr uint32_t Alignment = 0x10;

    enum class LocationType : uint32_t {
        Cpu,
        Dsp,
    };

    uint64_t CpuAddress = 0;
    // The DSP address associated to the MemoryPoolInfo (0 when not mapped).
    uint64_t DspAddress = 0;
    uint64_t Size = 0;
    LocationType Location = LocationType::Cpu;
    bool IsUsed = false;

    static MemoryPoolInfo* Null() { return nullptr; }

    static MemoryPoolInfo Create(LocationType location) {
        MemoryPoolInfo info;
        info.CpuAddress = 0;
        info.DspAddress = 0;
        info.Size = 0;
        info.Location = location;
        return info;
    }

    void SetCpuAddress(uint64_t cpuAddress, uint64_t size) {
        CpuAddress = cpuAddress;
        Size = size;
    }

    bool Contains(uint64_t targetCpuAddress, uint64_t size) const {
        if (CpuAddress <= targetCpuAddress && size + targetCpuAddress <= Size + CpuAddress) {
            return true;
        }

        return false;
    }

    // Translate the given CPU address to a DSP address (0 when not contained or not mapped).
    uint64_t Translate(uint64_t targetCpuAddress, uint64_t size) const {
        if (Contains(targetCpuAddress, size) && IsMapped()) {
            uint64_t offset = targetCpuAddress - CpuAddress;

            return DspAddress + offset;
        }

        return 0;
    }

    bool IsMapped() const { return DspAddress != 0; }
};

}
