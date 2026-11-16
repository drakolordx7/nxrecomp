// SPDX-License-Identifier: GPL-3.0-or-later
// Linux implementation of nxrt/os/procinfo.h: /proc/self/status, getrusage.
#include "nxrt/os/procinfo.h"

#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nxrt/os/mem.h"
#include "nxrt/os/thread.h"

namespace nxrt::os {
namespace {

// The number (in kB) of a "Name:   1234 kB" line of /proc/self/status, in bytes; 0 when absent.
uint64_t status_kb(const char* text, const char* name) {
    const size_t n = std::strlen(name);
    for (const char* p = text; p && *p;) {
        if (std::strncmp(p, name, n) == 0 && p[n] == ':') return std::strtoull(p + n + 1, nullptr, 10) * 1024ull;
        p = std::strchr(p, '\n');
        if (p) ++p;
    }
    return 0;
}

}  // namespace

bool process_stats(ProcessStats* out) {
    *out = ProcessStats{};
    bool ok = true;
    char buf[4096];
    if (std::FILE* f = std::fopen("/proc/self/status", "r")) {
        const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        std::fclose(f);
        out->working_set = status_kb(buf, "VmRSS");
        out->peak_working_set = status_kb(buf, "VmHWM");
        out->commit_bytes = status_kb(buf, "VmData");
    } else {
        ok = false;
    }
    out->private_bytes = mem::footprint();
    rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0)
        out->cpu_ms = static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 + static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
    else
        ok = false;
    out->handles = static_cast<uint32_t>(process_handle_count());
    out->threads = static_cast<uint32_t>(process_thread_count());
    return ok;
}

}  // namespace nxrt::os
