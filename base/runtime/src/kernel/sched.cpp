// SPDX-License-Identifier: GPL-3.0-or-later
// Console core model. The three application cores of the console are three host processors; every guest thread's affinity is its
// core mask mapped onto them and its HOS priority is mapped onto six host priority bands (os::PriorityBand) with the dynamic boost off.
// Measured in smo-recomp (runtime/os/os_threads.c): free scheduling failed 4 of 6 boots in sead heap code, the console model 0 of 6.
// Config: [sched] console_cores (default 1; env NXRT_SCHED_CONSOLE_CORES), [sched] host_cpus = hexadecimal mask of host processors to use,
// [sched] pick = auto (the three fastest free physical cores, one hardware thread each) | top (the old rule: every second processor from the top of the mask).
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/os/thread.h"

namespace nxrt::kernel {
namespace {

std::mutex g_lock;
bool g_done = false;
SchedInfo g_info;
void* g_slot = nullptr;            // the claim on this process's core slot (os::slot_claim)

uint64_t take_core_slot(const std::vector<uint64_t>& cores, int* slot_out) {
    for (size_t k = 0; k + 3 <= cores.size(); k += 3) {
        char name[64];
        snprintf(name, sizeof name, "nxrt_core_slot_%zu", k / 3);
        void* m = nullptr;
        const os::SlotResult r = os::slot_claim(name, &m);
        if (r == os::SlotResult::Error) break;
        if (r == os::SlotResult::Busy) continue;
        if (g_slot) os::slot_release(g_slot);
        g_slot = m;
        const uint64_t mask = cores[k] | cores[k + 1] | cores[k + 2];
        *slot_out = static_cast<int>(k / 3);
        NXLOG_I("sched", "console cores: slot %zu of %zu, the %zu-th to %zu-th fastest physical cores (mask %llx); other nxrt processes take other slots", k / 3, cores.size() / 3, k + 1, k + 3, (unsigned long long)mask);
        return mask;
    }
    return 0;
}

// The first free slot of six processors counted from the top of `proc` (a named mutex per slot, held while the process lives), so
// several games or test processes running side by side do not share processors. One process alone takes slot 0.
uint64_t take_slot(uint64_t proc, int* slot_out) {
    int bits[64], nb = 0;
    for (int b = 63; b >= 0; b--)
        if (proc >> b & 1) bits[nb++] = b;
    for (int k = 0; k < nb / 6; k++) {
        char name[64];
        snprintf(name, sizeof name, "nxrt_console_cores_%d", k);
        void* m = nullptr;
        const os::SlotResult r = os::slot_claim(name, &m);
        if (r == os::SlotResult::Error) break;
        if (r == os::SlotResult::Busy) continue;
        if (g_slot) os::slot_release(g_slot);
        g_slot = m;
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
    const uint64_t proc = os::process_cpu_mask();
    uint64_t usable = proc;
    std::string cpus = config_get_str("sched", "host_cpus", "");
    int slot = -1;
    if (!cpus.empty()) {
        uint64_t want = strtoull(cpus.c_str(), nullptr, 16);
        if (want & usable) usable &= want;
        NXLOG_I("sched", "host_cpus %s: using processors %llx of %llx", cpus.c_str(), (unsigned long long)usable, (unsigned long long)proc);
    } else {
        bool top = config_get_str("sched", "pick", "auto") == "top";
        uint64_t chosen = 0;
        if (!top) {
            const std::vector<uint64_t> cores = os::cores_fastest_first(usable);
            if (cores.size() >= 3) chosen = take_core_slot(cores, &slot);
        }
        if (chosen) {
            int k = 0;
            for (int b = 63; b >= 0 && k < 3; b--)
                if (chosen >> b & 1) g_info.host_masks[k++] = 1ull << b;
            g_info.slot = slot;
            g_info.active = k == 3;
            NXLOG_I("sched", "console core model ON: host masks %llx %llx %llx for cores 0 1 2", (unsigned long long)g_info.host_masks[0],
                    (unsigned long long)g_info.host_masks[1], (unsigned long long)g_info.host_masks[2]);
            return;
        }
        usable = take_slot(usable, &slot);
    }
    int n = 0;
    for (int b = 63; b >= 0 && n < 3; b--)
        if (usable >> b & 1) {
            g_info.host_masks[n++] = 1ull << b;
            b--;
        }
    g_info.slot = slot;
    g_info.active = n == 3;
    if (!g_info.active) {
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

}

SchedInfo sched_info() {
    std::lock_guard<std::mutex> lk(g_lock);
    init_locked();
    return g_info;
}

void sched_reset_for_tests() {
    std::lock_guard<std::mutex> lk(g_lock);
    g_done = false;
    g_info = SchedInfo{};
    if (g_slot) { os::slot_release(g_slot); g_slot = nullptr; }
}

// nn::os priorities run 0 (highest) .. 31 (lowest); HOS priority = nn::os + 28. Six bands under the model:
// <= 4, 5..8, 9..15, 16, 17, 18..31 (smo: 17 against 18 on one core decided a use-after-free race in 2 of 91 stage-entry runs).
os::PriorityBand sched_host_priority(int hos_prio) {
    using B = os::PriorityBand;
    const int nn = hos_prio - 28;
    if (sched_info().active) {
        if (nn <= 4) return B::TimeCritical;
        if (nn <= 8) return B::Highest;
        if (nn <= 15) return B::AboveNormal;
        if (nn == 16) return B::Normal;
        if (nn == 17) return B::BelowNormal;
        return B::Lowest;
    }
    if (nn <= 4) return B::Highest;
    if (nn <= 15) return B::AboveNormal;
    if (nn == 16) return B::Normal;
    if (nn <= 27) return B::BelowNormal;
    return B::Lowest;
}

void sched_set_process_priority(bool interactive) {
    std::string v = config_get_str("sched", "priority_class", "auto");
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    os::ProcessPriority cls = os::ProcessPriority::Normal;
    const char* label = "normal";
    bool set = true;
    if (v == "high") { cls = os::ProcessPriority::High; label = "high"; }
    else if (v == "above_normal" || v == "above") { cls = os::ProcessPriority::AboveNormal; label = "above normal"; }
    else if (v == "normal") {}
    else if (v == "auto" && interactive) { cls = os::ProcessPriority::High; label = "high"; }
    else {
        set = false;
        if (v != "auto") NXLOG_W("sched", "[sched] priority_class = %s is not auto, high, above_normal or normal: left alone", v.c_str());
    }
    if (!set) return;
    if (os::process_set_priority(cls)) NXLOG_I("sched", "process priority class: %s", label);
    else NXLOG_W("sched", "raising the process priority (%s) failed or is not permitted", label);
}

void yield_host() {
    KThread* t = current_thread();
    if (t && t->aged.load(std::memory_order_relaxed)) os::cpu_relax();   // aged: a hand-over would give its time slice to the program that starves it
    else os::thread_yield();
}

int sched_current_core() {
    SchedInfo si = sched_info();
    KThread* t = current_thread();
    if (si.active) {
        const int cpu = os::cpu_current();
        if (cpu >= 0)
            for (int i = 0; i < 3; i++)
                if (si.host_masks[i] >> cpu & 1) return i;
    }
    return t ? (t->ideal_core >= 0 ? t->ideal_core : 0) : 0;
}

os::ThreadPlacement sched_placement(const KThread& t) {
    os::ThreadPlacement p;
    const SchedInfo si = sched_info();
    if (si.active) {
        uint64_t m = 0;
        for (int i = 0; i < 3; i++)
            if (t.core_mask >> i & 1) m |= si.host_masks[i];
        if (!m) m = si.host_masks[0];
        p.affinity = m;
        p.disable_boost = true;                    // the host's dynamic boost off
    }
    p.set_band = true;
    p.band = sched_host_priority(t.prio);
    return p;
}

void sched_apply(KThread& t) {
    if (!t.host_handle) return;
    if (!os::thread_place(t.host_handle, sched_placement(t))) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            NXLOG_W("sched", "the host refused part of a thread's placement (priority bands above the process's nice value need CAP_SYS_NICE or an RLIMIT_NICE on Linux)");
    }
}

}
