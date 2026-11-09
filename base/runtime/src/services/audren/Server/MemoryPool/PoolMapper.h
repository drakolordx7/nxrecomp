// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/MemoryPool/PoolMapper.cs (Ryubing fork, MIT).
//
// Mapping model: there is no DSP-side page table here. Guest memory is host memory and a memory pool's DSP address equals its
// CPU address, so every "map" of a region is the identity. The C# calls into AudioProcessorMemoryManager are kept as the static
// helpers ProcessorMap / ProcessorUnmap below, so the surrounding logic reads the same as the C#.
#pragma once
#include <cstdio>

#include "../../Constants.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "AddressInfo.h"
#include "MemoryPoolInfo.h"

namespace nxrt::audren {

class PoolMapper {
  public:
    // Pseudo handle of the current process (C# CurrentProcessPseudoHandle).
    static constexpr uint32_t CurrentProcessPseudoHandle = 0xFFFF8001;

    enum class UpdateResult : uint32_t {
        Success = 0,
        InvalidParameter = 1,
        MapError = 2,
        UnmapError = 3,
    };

  private:
    uint32_t _processHandle;
    Span<MemoryPoolInfo> _memoryPools;
    // If set to true, this will try to force map memory pool even if their state are considered invalid.
    bool _isForceMapEnabled;

    // C# AudioProcessorMemoryManager.Map: identity mapping, 0 for an address that cannot be mapped.
    static uint64_t ProcessorMap(uint32_t processHandle, uint64_t cpuAddress, uint64_t size) {
        (void)processHandle;
        (void)size;
        return cpuAddress;
    }

    // C# AudioProcessorMemoryManager.Unmap: nothing to undo with identity mapping.
    static void ProcessorUnmap(uint32_t processHandle, uint64_t cpuAddress, uint64_t size) {
        (void)processHandle;
        (void)cpuAddress;
        (void)size;
    }

    Span<MemoryPoolInfo> FindMemoryPool(uint64_t cpuAddress, uint64_t size) const {
        if (!_memoryPools.IsEmpty() && _memoryPools.Length > 0) {
            for (size_t i = 0; i < _memoryPools.Length; i++) {
                if (_memoryPools.Data[i].Contains(cpuAddress, size)) {
                    return _memoryPools.Slice(i, 1);
                }
            }
        }

        return Span<MemoryPoolInfo>();
    }

    bool AssignDspAddress(AddressInfo& addressInfo) {
        if (addressInfo.CpuAddress == 0) {
            return false;
        }

        if (_memoryPools.Length > 0) {
            Span<MemoryPoolInfo> memoryPool = FindMemoryPool(addressInfo.CpuAddress, addressInfo.Size);

            if (!memoryPool.IsEmpty()) {
                addressInfo.SetupMemoryPool(memoryPool);

                return true;
            }
        }

        if (_isForceMapEnabled) {
            uint64_t dspAddress = ProcessorMap(_processHandle, addressInfo.CpuAddress, addressInfo.Size);

            addressInfo.ForceMappedDspAddress = dspAddress;

            // The C# code calls AudioProcessorMemoryManager.Map a second time here, with the same arguments and no effect on the
            // result. That duplicate call is not ported.
        } else {
            addressInfo.SetupMemoryPool(MemoryPoolInfo::Null());
        }

        return false;
    }

  public:
    PoolMapper(uint32_t processHandle, bool isForceMapEnabled)
        : _processHandle(processHandle), _memoryPools(), _isForceMapEnabled(isForceMapEnabled) {}

    PoolMapper(uint32_t processHandle, Span<MemoryPoolInfo> memoryPool, bool isForceMapEnabled)
        : _processHandle(processHandle), _memoryPools(memoryPool), _isForceMapEnabled(isForceMapEnabled) {}

    // Initialize the MemoryPoolInfo for system use. Only Dsp-located pools are accepted.
    bool InitializeSystemPool(MemoryPoolInfo& memoryPool, uint64_t cpuAddress, uint64_t size) {
        if (memoryPool.Location != MemoryPoolInfo::LocationType::Dsp) {
            return false;
        }

        return InitializePool(memoryPool, cpuAddress, size);
    }

    bool InitializePool(MemoryPoolInfo& memoryPool, uint64_t cpuAddress, uint64_t size) {
        memoryPool.SetCpuAddress(cpuAddress, size);

        return Map(memoryPool) != 0;
    }

    uint32_t GetProcessHandle(const MemoryPoolInfo& memoryPool) const {
        if (memoryPool.Location == MemoryPoolInfo::LocationType::Cpu) {
            return CurrentProcessPseudoHandle;
        }

        if (memoryPool.Location == MemoryPoolInfo::LocationType::Dsp) {
            return _processHandle;
        }

        return 0;
    }

    // Map the MemoryPoolInfo on the processor. Returns the DSP address mapped (0 on failure).
    uint64_t Map(MemoryPoolInfo& memoryPool) {
        uint64_t result = ProcessorMap(GetProcessHandle(memoryPool), memoryPool.CpuAddress, memoryPool.Size);

        if (result != 0) {
            memoryPool.DspAddress = result;
        }

        return result;
    }

    // Unmap the MemoryPoolInfo from the processor. Returns false if the pool is still in use.
    bool Unmap(MemoryPoolInfo& memoryPool) {
        if (memoryPool.IsUsed) {
            return false;
        }

        ProcessorUnmap(GetProcessHandle(memoryPool), memoryPool.CpuAddress, memoryPool.Size);

        memoryPool.SetCpuAddress(0, 0);
        memoryPool.DspAddress = 0;

        return true;
    }

    void ForceUnmap(AddressInfo& addressInfo) {
        if (_isForceMapEnabled) {
            Span<MemoryPoolInfo> memoryPool = FindMemoryPool(addressInfo.CpuAddress, addressInfo.Size);

            if (!memoryPool.IsEmpty()) {
                ProcessorUnmap(_processHandle, memoryPool.Data[0].CpuAddress, memoryPool.Data[0].Size);

                return;
            }

            ProcessorUnmap(_processHandle, addressInfo.CpuAddress, 0);
        }
    }

    bool TryAttachBuffer(BehaviourParameter::ErrorInfo& errorInfo, AddressInfo& addressInfo, uint64_t cpuAddress, uint64_t size) {
        errorInfo = BehaviourParameter::ErrorInfo{};

        addressInfo.Setup(cpuAddress, size);

        if (AssignDspAddress(addressInfo)) {
            errorInfo.error_code = ResultCode::Success;
            errorInfo.extra_error_info = 0;

            return true;
        }

        errorInfo.error_code = ResultCode::InvalidAddressInfo;
        errorInfo.extra_error_info = addressInfo.CpuAddress;

        return _isForceMapEnabled;
    }

    UpdateResult Update(MemoryPoolInfo& memoryPool, const MemoryPoolInParameter& inParameter, MemoryPoolOutStatus& outStatus) {
        const MemoryPoolUserState inputState = inParameter.state;

        MemoryPoolUserState outputState = MemoryPoolUserState::Invalid;

        constexpr uint32_t PageSize = 0x1000;

        if (inputState != MemoryPoolUserState::RequestAttach && inputState != MemoryPoolUserState::RequestDetach) {
            return UpdateResult::Success;
        }

        if (inParameter.cpu_address == 0 || (inParameter.cpu_address % PageSize) != 0) {
            return UpdateResult::InvalidParameter;
        }

        if (inParameter.size == 0 || (inParameter.size % PageSize) != 0) {
            return UpdateResult::InvalidParameter;
        }

        if (inputState == MemoryPoolUserState::RequestAttach) {
            bool initializeSuccess = InitializePool(memoryPool, inParameter.cpu_address, inParameter.size);

            if (!initializeSuccess) {
                memoryPool.SetCpuAddress(0, 0);

                std::fprintf(stderr, "Map of memory pool (address: 0x%llx, size 0x%llx) failed!\n",
                             static_cast<unsigned long long>(inParameter.cpu_address), static_cast<unsigned long long>(inParameter.size));
                return UpdateResult::MapError;
            }

            outputState = MemoryPoolUserState::Attached;
        } else {
            if (memoryPool.CpuAddress != inParameter.cpu_address || memoryPool.Size != inParameter.size) {
                return UpdateResult::InvalidParameter;
            }

            if (!Unmap(memoryPool)) {
                std::fprintf(stderr, "Unmap of memory pool (address: 0x%llx, size 0x%llx) failed!\n",
                             static_cast<unsigned long long>(memoryPool.CpuAddress), static_cast<unsigned long long>(memoryPool.Size));
                return UpdateResult::UnmapError;
            }

            outputState = MemoryPoolUserState::Detached;
        }

        outStatus.state = outputState;

        return UpdateResult::Success;
    }

    static void ClearUsageState(Span<MemoryPoolInfo> memoryPool) {
        for (size_t i = 0; i < memoryPool.Length; i++) {
            memoryPool.Data[i].IsUsed = false;
        }
    }
};

}
