// SPDX-License-Identifier: GPL-3.0-or-later
// Windows implementation of nxrt/os/procinfo.h.
#include "nxrt/os/procinfo.h"

#include <windows.h>
#include <psapi.h>

#include "nxrt/os/thread.h"

namespace nxrt::os {

bool process_stats(ProcessStats* out) {
    *out = ProcessStats{};
    HANDLE self = GetCurrentProcess();
    bool ok = true;
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    if (GetProcessMemoryInfo(self, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm)) {
        out->working_set = pm.WorkingSetSize;
        out->private_bytes = pm.PrivateUsage;
        out->peak_working_set = pm.PeakWorkingSetSize;
        out->commit_bytes = pm.PagefileUsage;
    } else {
        ok = false;
    }
    FILETIME c, e, k, u;
    if (GetProcessTimes(self, &c, &e, &k, &u))
        out->cpu_ms = static_cast<double>(((static_cast<uint64_t>(k.dwHighDateTime) << 32) | k.dwLowDateTime) + ((static_cast<uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime)) / 10000.0;
    else
        ok = false;
    out->handles = static_cast<uint32_t>(process_handle_count());
    out->threads = static_cast<uint32_t>(process_thread_count());
    out->gdi = GetGuiResources(self, GR_GDIOBJECTS);
    out->user = GetGuiResources(self, GR_USEROBJECTS);
    return ok;
}

}  // namespace nxrt::os
