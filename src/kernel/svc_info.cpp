// SPDX-License-Identifier: GPL-3.0-or-later
// Semantics adapted from SuperMarioWonderRecomp runtime/src/svc_process.cpp (MIT, Copyright (c) 2026 BelmanteGu); see THIRD_PARTY_NOTICES.md.
// Info SVCs (include/nxrt/svc.h): GetInfo 0x29, GetSystemInfo 0x6F, GetResourceLimitLimitValue 0x30, GetResourceLimitCurrentValue 0x31,
// GetResourceLimitPeakValue 0x37, GetProcessId 0x24, GetThreadId 0x25, GetProcessInfo 0x7C.
// AArch64 columns per Switchbrew: inputs in x1.., result in w0, outputs in x1.
#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services.h"
#include "nxrt/svc.h"

namespace nxrt {
namespace {

constexpr uint32_t kInvalidCombination = 0xE801;   // kernel result 116 (InvalidCombination); not in kernel.h yet

// Values Switchbrew does not document (it gives names only) are nxrt choices; the comment on each says where a value comes from.
constexpr uint64_t kCoreMaskApp = 0x7;                          // nxrt: cores 0..2, the three application cores
constexpr uint64_t kPriorityMaskApp = 0x0FFFFFFFF0000000ull;    // nxrt: bits 28..59 = HOS priorities 0x1C..0x3B (kernel.h: applications 28..59)
constexpr uint64_t kAslrRegionAddress = 0x0008000000ull;       // brief value (Switchbrew: no value given)
constexpr uint64_t kAslrRegionSize = 0x7FF8000000ull;           // brief value (Switchbrew: no value given)
constexpr uint64_t kTotalMemoryAvailable = 0xCD500000ull;       // brief value: ~3.2 GiB, like an application
constexpr uint64_t kUsedMemoryOverhead = 0x4000000ull;          // brief value: 64 MiB added to the heap size for "used"
constexpr uint64_t kEntropyDefaultSeed = 0x1234567890ABCDEFull; // brief value
constexpr uint64_t kGoldenRatio64 = 0x9E3779B97F4A7C15ull;      // used to separate the four RandomEntropy subtypes
constexpr uint64_t kDefaultProcessId = 0x51;                    // brief value (config [process] pid)
constexpr uint32_t kInfoThreadTickCountOld = 0xF0000002u;       // ThreadTickCount id on firmware 1.0.0-12.1.0 (Switchbrew)
constexpr uint32_t kInfoThreadTickCount = 25;                   // ThreadTickCount id on firmware 12.1.0+ (Switchbrew table)
constexpr uint64_t kProcessStateStarted = 2;                    // Switchbrew ProcessState 2 = Started (not debugged)

uint64_t used_memory() { return svc_current_heap_size() + kUsedMemoryOverhead; }

// UserExceptionContextAddress: one committed kernel page, made on first use and again only if it has been unmapped.
uint64_t user_exception_context() {
    static uint64_t page = 0;
    if (page == 0 || !mem::is_mapped(page, mem::kPage)) page = mem::alloc_kernel(mem::kPage);
    return page;
}

// Returns the result code; `out` receives the value (0 on error).
uint32_t get_info(uint32_t type, uint32_t handle, uint64_t sub, uint64_t& out) {
    out = 0;
    switch (type) {
        case 0: out = kCoreMaskApp; return kernel::kOk;                       // CoreMask
        case 1: out = kPriorityMaskApp; return kernel::kOk;                   // PriorityMask
        case 2: out = mem::kAliasBase; return kernel::kOk;                    // AliasRegionAddress
        case 3: out = mem::kAliasSize; return kernel::kOk;                    // AliasRegionSize
        case 4: out = mem::kHeapBase; return kernel::kOk;                     // HeapRegionAddress
        case 5: out = mem::kHeapMax; return kernel::kOk;                      // HeapRegionSize
        case 6: out = kTotalMemoryAvailable; return kernel::kOk;              // TotalMemorySize
        case 7: out = used_memory(); return kernel::kOk;                      // UsedMemorySize
        case 8: out = 0; return kernel::kOk;                                  // DebuggerAttached: never
        case 9: out = 0; return kernel::kOk;                                  // ResourceLimit: no handle
        case 10: out = 0; return kernel::kOk;                                 // IdleTickCount (nxrt: no idle accounting)
        case 11: {                                                            // RandomEntropy, subtype 0..3
            if (sub > 3) return kInvalidCombination;
            uint64_t seed = static_cast<uint64_t>(config_get_int("process", "entropy_seed", static_cast<int64_t>(kEntropyDefaultSeed)));
            out = seed ^ (sub * kGoldenRatio64);
            return kernel::kOk;
        }
        case 12: out = kAslrRegionAddress; return kernel::kOk;                // AslrRegionAddress
        case 13: out = kAslrRegionSize; return kernel::kOk;                   // AslrRegionSize
        case 14: out = mem::kStackBase; return kernel::kOk;                   // StackRegionAddress
        case 15: out = mem::kStackSize; return kernel::kOk;                   // StackRegionSize
        case 16: out = static_cast<uint64_t>(config_get_int("process", "system_resource_size", 0)); return kernel::kOk;
        case 17: out = 0; return kernel::kOk;                                 // SystemResourceSizeUsed
        case 18:                                                              // ProgramId: set by the game layer ([process] program_id)
            out = services::program_id_now();   // [process] program_id, else [title] program_id (default: the title the profile describes)
            return kernel::kOk;
        case 20:                                                              // UserExceptionContextAddress
            out = user_exception_context();
            return out ? kernel::kOk : kernel::kOutOfMemory;
        case 21: out = kTotalMemoryAvailable; return kernel::kOk;             // TotalNonSystemMemorySize (same as 6)
        case 22: out = used_memory(); return kernel::kOk;                     // UsedNonSystemMemorySize (same as 7)
        case 23: out = 1; return kernel::kOk;                                 // IsApplication
        case kInfoThreadTickCountOld:
        case kInfoThreadTickCount:
            // ThreadTickCount needs a thread handle (or the pseudo handle of the calling thread); the tick value itself
            // is 0 for now: kernel::system_tick() is declared in kernel.h but not defined on this branch.
            // TODO(lead): return kernel::system_tick() (or the per-core/total tick of the thread) once it is implemented.
            if (handle != kernel::kPseudoCurrentThread && !kernel::handle_get(handle)) return kernel::kInvalidHandle;
            out = 0;
            return kernel::kOk;
        default:
            NXLOG_W("svc", "svcGetInfo: unknown type %u sub %llu", type, static_cast<unsigned long long>(sub));
            return kInvalidCombination;
    }
}

void info_get_info(::Ctx* c) {
    uint64_t value = 0;
    uint32_t rc = get_info(static_cast<uint32_t>(reg(c, 1)), static_cast<uint32_t>(reg(c, 2)), reg(c, 3), value);
    reg(c, 1) = value;
    svc_ret(c, rc);
}

// GetSystemInfo: the handle (x2) is unused, the subtype (x3) selects the memory pool or the pid range bound.
uint32_t get_system_info(uint32_t type, uint64_t sub, uint64_t& out) {
    out = 0;
    switch (type) {
        case 0:                                                // TotalPhysicalMemorySize per pool
            switch (sub) {
                case 0: out = 0x100000000ull; return kernel::kOk;   // Application: 4 GiB (brief value)
                case 1: out = 0x20000000ull; return kernel::kOk;    // Applet: 512 MiB (nxrt choice)
                case 2: out = 0x10000000ull; return kernel::kOk;    // System: 256 MiB (nxrt choice)
                case 3: out = 0x10000000ull; return kernel::kOk;    // SystemUnsafe: 256 MiB (nxrt choice)
                default: return kInvalidCombination;
            }
        case 1:                                                // UsedPhysicalMemorySize per pool: only the application pool is in use
            if (sub == 0) { out = used_memory(); return kernel::kOk; }
            if (sub <= 3) return kernel::kOk;                  // other pools: 0
            return kInvalidCombination;
        case 2:                                                // InitialProcessIdRange: subtype 0 lower bound, 1 upper bound
            // nxrt choice: the range is the single pid GetProcessId reports.
            if (sub == 0 || sub == 1) { out = kDefaultProcessId; return kernel::kOk; }
            return kInvalidCombination;
        default:
            return kInvalidCombination;
    }
}

void system_info_get(::Ctx* c) {
    uint64_t value = 0;
    uint32_t rc = get_system_info(static_cast<uint32_t>(reg(c, 1)), reg(c, 3), value);
    reg(c, 1) = value;
    svc_ret(c, rc);
}

// LimitableResource: 0 PhysicalMemoryMax, 1 ThreadCountMax, 2 EventCountMax, 3 TransferMemoryCountMax, 4 SessionCountMax.
bool resource_limit(uint64_t which, uint64_t& v) {
    switch (which) {
        case 0: v = kTotalMemoryAvailable; return true;   // brief value
        case 1: v = 0x200; return true;                   // nxrt choice
        case 2: v = 0x600; return true;                   // nxrt choice
        case 3: v = 0x80; return true;                    // nxrt choice
        case 4: v = 0x80; return true;                    // nxrt choice
        default: return false;
    }
}

bool resource_current(uint64_t which, uint64_t& v) {
    switch (which) {
        case 0: v = svc_current_heap_size(); return true;
        // TODO(lead): kernel::threads().size() once kernel::threads() is implemented; thread_count_running() is declared but not
        // defined on this branch, so the calling thread is reported as the only thread for now.
        case 1: v = 1; return true;
        case 2: case 3: case 4: v = 0; return true;
        default: return false;
    }
}

// Peak values are not tracked by nxrt yet: the peak equals the current value.
void resource_limit_value(::Ctx* c) {
    uint64_t v = 0;
    bool ok = resource_limit(reg(c, 2), v);
    reg(c, 1) = ok ? v : 0;
    svc_ret(c, ok ? kernel::kOk : kInvalidCombination);
}

void resource_current_value(::Ctx* c) {
    uint64_t v = 0;
    bool ok = resource_current(reg(c, 2), v);
    reg(c, 1) = ok ? v : 0;
    svc_ret(c, ok ? kernel::kOk : kInvalidCombination);
}

void resource_peak_value(::Ctx* c) {
    uint64_t v = 0;
    bool ok = resource_current(reg(c, 2), v);
    reg(c, 1) = ok ? v : 0;
    svc_ret(c, ok ? kernel::kOk : kInvalidCombination);
}

// GetProcessId(process handle): only the pseudo handle of the current process is accepted (no other process objects exist yet).
void get_process_id(::Ctx* c) {
    if (static_cast<uint32_t>(reg(c, 1)) != kernel::kPseudoCurrentProcess) {
        reg(c, 1) = 0;
        svc_ret(c, kernel::kInvalidHandle);
        return;
    }
    reg(c, 1) = static_cast<uint64_t>(config_get_int("process", "pid", static_cast<int64_t>(kDefaultProcessId)));
    svc_ret(c, kernel::kOk);
}

// GetThreadId(thread handle). The pseudo handle resolves to the calling guest thread through kernel::handle_get.
void get_thread_id(::Ctx* c) {
    auto t = kernel::handle_as<kernel::KThread>(static_cast<uint32_t>(reg(c, 1)));
    if (!t) {
        reg(c, 1) = 0;
        svc_ret(c, kernel::kInvalidHandle);
        return;
    }
    reg(c, 1) = t->id;
    svc_ret(c, kernel::kOk);
}

// GetProcessInfo(process handle, ProcessInfoType): type 0 is ProcessState. The handle is not checked (no process objects yet).
void get_process_info(::Ctx* c) {
    if (static_cast<uint32_t>(reg(c, 2)) == 0) {
        reg(c, 1) = kProcessStateStarted;
        svc_ret(c, kernel::kOk);
        return;
    }
    reg(c, 1) = 0;
    svc_ret(c, kInvalidCombination);
}

}  // namespace

void svc_register_info_group() {
    svc_register(0x29, info_get_info);
    svc_register(0x6F, system_info_get);
    svc_register(0x30, resource_limit_value);
    svc_register(0x31, resource_current_value);
    svc_register(0x37, resource_peak_value);
    svc_register(0x24, get_process_id);
    svc_register(0x25, get_thread_id);
    svc_register(0x7C, get_process_info);
}

}  // namespace nxrt
