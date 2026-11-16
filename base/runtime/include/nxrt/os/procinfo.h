// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os procinfo: what the host says about this process, for the soak sampler and the test tooling. Windows: GetProcessMemoryInfo,
// GetProcessTimes, GetProcessHandleCount, GetGuiResources. Linux: /proc/self/status, getrusage, /proc/self/fd and task.
#pragma once
#include <cstdint>

namespace nxrt::os {

struct ProcessStats {
    uint64_t working_set = 0;        // resident bytes
    uint64_t private_bytes = 0;      // memory that is not shared with the file cache (Windows PrivateUsage; Linux anonymous + shmem pages)
    uint64_t peak_working_set = 0;   // the most resident bytes so far
    uint64_t commit_bytes = 0;       // memory the process has committed (Windows PagefileUsage; Linux VmData: private writable mappings)
    double cpu_ms = 0;               // user plus kernel CPU time of all threads, ms
    uint32_t handles = 0;            // open handles (Windows) or file descriptors (Linux)
    uint32_t threads = 0;
    uint32_t gdi = 0, user = 0;      // GDI and USER objects (Windows only, 0 elsewhere)
};
// False when the host would not say (the fields it could read are still filled).
bool process_stats(ProcessStats* out);

}  // namespace nxrt::os
