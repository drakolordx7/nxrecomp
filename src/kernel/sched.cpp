// SPDX-License-Identifier: GPL-3.0-or-later
// Console core model. The three application cores of the console are three host processors; every guest thread's affinity is its
// core mask mapped onto them and its HOS priority is mapped onto six Windows priority bands with the dynamic boost off.
// Measured in smo-recomp (runtime/os/os_threads.c): free scheduling failed 4 of 6 boots in sead heap code, the console model 0 of 6.
// Config: [sched] console_cores (default 1; env NXRT_SCHED_CONSOLE_CORES), [sched] host_cpus = hexadecimal mask of host processors to use.
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"

namespace nxrt::kernel {
namespace {

std::mutex g_lock;
bool g_done = false;
SchedInfo g_info;
HANDLE g_slot_mutex = nullptr;

// The first free slot of six processors counted from the top of `proc` (a named mutex per slot, held while the process lives), so
// several games or test processes running side by side do not share processors. One process alone takes slot 0.
uint64_t take_slot(uint64_t proc, int* slot_out) {
    int bits[64], nb = 0;
    for (int b = 63; b >= 0; b--)
        if (proc >> b & 1) bits[nb++] = b;
    for (int k = 0; k < nb / 6; k++) {
        char name[64];
        snprintf(name, sizeof name, "Local\\nxrt_console_cores_%d", k);
        HANDLE m = CreateMutexA(nullptr, FALSE, name);
        if (!m) break;
        if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(m); continue; }
        if (g_slot_mutex) CloseHandle(g_slot_mutex);
        g_slot_mutex = m;
        uint64_t mask = 0;
        for (int i = 6 * k; i < 6 * k + 6; i++) mask |= 1ull << bits[i];
        *slot_out = k;
        NXLOG_I("sched", "console cores in processor slot %d of %d (mask %llx); other nxrt processes take other slots", k, nb / 6, (unsigned long long)mask);
        return mask;
    }
    NXLOG_W("sched", "every processor slot is held by other processes: sharing the top of the affinity mask");
    *slot_out = -1;
    return proc;
}

void init_locked() {
    if (g_done) return;
    g_done = true;
    g_info = SchedInfo{};
    if (!config_get_bool("sched", "console_cores", true)) {
        NXLOG_I("sched", "console core model off ([sched] console_cores = 0)");
        return;
    }
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    uint64_t usable = proc;
    std::string cpus = config_get_str("sched", "host_cpus", "");
    int slot = -1;
    if (!cpus.empty()) {
        uint64_t want = _strtoui64(cpus.c_str(), nullptr, 16);
        if (want & usable) usable &= want;
        NXLOG_I("sched", "host_cpus %s: using processors %llx of %llx", cpus.c_str(), (unsigned long long)usable, (unsigned long long)proc);
    } else {
        usable = take_slot(usable, &slot);
    }
    int n = 0;
    for (int b = 63; b >= 0 && n < 3; b--)      // from the top, every second processor: not two hardware threads of one core
        if (usable >> b & 1) {
            g_info.host_masks[n++] = 1ull << b;
            b--;
        }
    g_info.slot = slot;
    g_info.active = n == 3;
    if (!g_info.active) {                       // second chance without skipping: any three processors
        n = 0;
        for (int b = 63; b >= 0 && n < 3; b--)
            if (usable >> b & 1) g_info.host_masks[n++] = 1ull << b;
        g_info.active = n == 3;
    }
    if (g_info.active)
        NXLOG_I("sched", "console core model ON: host masks %llx %llx %llx for cores 0 1 2", (unsigned long long)g_info.host_masks[0],
                (unsigned long long)g_info.host_masks[1], (unsigned long long)g_info.host_masks[2]);
    else
        NXLOG_W("sched", "console core model NOT possible: fewer than 3 usable host processors");
}

}  // namespace

SchedInfo sched_info() {
    std::lock_guard<std::mutex> lk(g_lock);
    init_locked();
    return g_info;
}

void sched_reset_for_tests() {
    std::lock_guard<std::mutex> lk(g_lock);
    g_done = false;
    g_info = SchedInfo{};
    if (g_slot_mutex) { CloseHandle(g_slot_mutex); g_slot_mutex = nullptr; }     // so the next sched_info() can take the same slot again
}

// nn::os priorities run 0 (highest) .. 31 (lowest); HOS priority = nn::os + 28. Six bands under the model:
// <= 4, 5..8, 9..15, 16, 17, 18..31 (smo: 17 against 18 on one core decided a use-after-free race in 2 of 91 stage-entry runs).
int sched_host_priority(int hos_prio) {
    const int nn = hos_prio - 28;
    if (sched_info().active) {
        if (nn <= 4) return THREAD_PRIORITY_TIME_CRITICAL;
        if (nn <= 8) return THREAD_PRIORITY_HIGHEST;
        if (nn <= 15) return THREAD_PRIORITY_ABOVE_NORMAL;
        if (nn == 16) return THREAD_PRIORITY_NORMAL;
        if (nn == 17) return THREAD_PRIORITY_BELOW_NORMAL;
        return THREAD_PRIORITY_LOWEST;
    }
    if (nn <= 4) return THREAD_PRIORITY_HIGHEST;
    if (nn <= 15) return THREAD_PRIORITY_ABOVE_NORMAL;
    if (nn == 16) return THREAD_PRIORITY_NORMAL;
    if (nn <= 27) return THREAD_PRIORITY_BELOW_NORMAL;
    return THREAD_PRIORITY_LOWEST;
}

void sched_set_process_priority(bool interactive) {
    std::string v = config_get_str("sched", "priority_class", "auto");
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    DWORD cls = 0;
    if (v == "high") cls = HIGH_PRIORITY_CLASS;
    else if (v == "above_normal" || v == "above") cls = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (v == "normal") cls = NORMAL_PRIORITY_CLASS;
    else if (v == "auto") cls = interactive ? HIGH_PRIORITY_CLASS : 0;
    else NXLOG_W("sched", "[sched] priority_class = %s is not auto, high, above_normal or normal: left alone", v.c_str());
    if (!cls) return;
    if (SetPriorityClass(GetCurrentProcess(), cls)) NXLOG_I("sched", "process priority class: %s", cls == HIGH_PRIORITY_CLASS ? "high" : cls == ABOVE_NORMAL_PRIORITY_CLASS ? "above normal" : "normal");
    else NXLOG_W("sched", "SetPriorityClass failed, error %lu", static_cast<unsigned long>(GetLastError()));
}

int sched_current_core() {
    SchedInfo si = sched_info();
    KThread* t = current_thread();
    if (si.active) {
        PROCESSOR_NUMBER pn{};
        GetCurrentProcessorNumberEx(&pn);
        if (pn.Group == 0)
            for (int i = 0; i < 3; i++)
                if (si.host_masks[i] >> pn.Number & 1) return i;
    }
    return t ? (t->ideal_core >= 0 ? t->ideal_core : 0) : 0;
}

void sched_apply(KThread& t) {
    HANDLE h = static_cast<HANDLE>(t.host_handle);
    if (!h) return;
    SchedInfo si = sched_info();
    if (si.active) {
        DWORD_PTR m = 0;
        for (int i = 0; i < 3; i++)
            if (t.core_mask >> i & 1) m |= static_cast<DWORD_PTR>(si.host_masks[i]);
        if (!m) m = static_cast<DWORD_PTR>(si.host_masks[0]);
        SetThreadAffinityMask(h, m);
        SetThreadPriorityBoost(h, TRUE);            // Windows' dynamic boost off
    }
    SetThreadPriority(h, sched_host_priority(t.prio));
}

}  // namespace nxrt::kernel
