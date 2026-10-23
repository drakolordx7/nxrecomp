// SPDX-License-Identifier: GPL-3.0-or-later
// Aging for guest threads that the host scheduler leaves ready but never runs (S20, debug note 422).
//
// The console core model pins every guest thread to one host processor and maps the HOS priorities onto six Windows bands with the wake-up
// boost off (kernel/sched.cpp, debug notes 180 and 221). That keeps the console's strict priority order, and it has a price on a shared PC:
// the lowest bands rank at or below ordinary programs, so while a build or another program keeps the pinned processor busy, a thread in
// those bands stays ready and gets no time. The resource loader sits in the lowest band: with a normal-priority load on its processor the
// game's load never finishes (the frames keep coming, the loader sits in its decompress loop) and a scripted run reports a "hang".
// Windows' own remedy, a boost to 15 for two time slices once a thread has waited four seconds, is too slow for that.
//
// This guard asks the system which of our threads are in the Ready state (NtQuerySystemInformation: thread state and context-switch count).
// A guest thread that was ready at several polls in a row without a single context switch in between gets Windows' top normal priority for
// 450 ms (and its yields stop handing the processor over, which cost it most of a boost: a yield gives a time slice to any ready thread, a lower
// priority one included), then goes back to its band. Healthy runs never starve for that long, so nothing changes for them: it is an escape from
// starvation, not a second scheduler. Config: [sched] starvation_guard (default on), [sched] starvation_ms (default 300, time a thread must
// have been ready and unscheduled before it is boosted).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"

namespace nxrt::kernel {
namespace {

// The parts of the NtQuerySystemInformation(SystemProcessInformation) records that are used (stable since Windows Vista, x64 layout).
struct ThreadRec {
    LARGE_INTEGER kernel_time, user_time, create_time;
    ULONG wait_time;
    PVOID start_address;
    PVOID unique_process, unique_thread;
    LONG priority, base_priority;
    ULONG context_switches, thread_state, wait_reason;
};
static_assert(sizeof(ThreadRec) == 0x50, "SYSTEM_THREAD_INFORMATION");
struct ProcRec {
    ULONG next_entry_offset, number_of_threads;
    uint8_t body[0x100 - 8];       // the rest of SYSTEM_PROCESS_INFORMATION up to the thread array
};
static_assert(sizeof(ProcRec) == 0x100, "SYSTEM_PROCESS_INFORMATION");
constexpr ULONG kStateReady = 1;
constexpr ULONG kStateStandby = 3;
constexpr int kSystemProcessInformation = 5;

using NtQuery = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

struct Guard {
    std::mutex m;
    std::condition_variable cv;
    std::thread th;
    bool stop = false;
    std::atomic<uint64_t> boosts{0};
};

Guard& guard() {
    static Guard* g = new Guard;     // never destroyed: a joinable thread must not die with the static
    return *g;
}

struct Seen {
    ULONG switches = 0;
    int ready_polls = 0;
    int boost_polls = 0;
    bool boosted = false;
};
constexpr int kBoostPolls = 3;     // a boost lasts three polls (450 ms)

void loop(Guard* g, int poll_ms, int need_polls) {
    log_set_thread_name("starve-guard");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);        // it must run when everything else is starved
    NtQuery nt_query = reinterpret_cast<NtQuery>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation"));
    if (!nt_query) return;
    std::vector<uint8_t> buf(1 << 20);
    std::unordered_map<uint32_t, Seen> seen;       // by host thread id
    const uint32_t pid = GetCurrentProcessId();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g->m);
            if (g->cv.wait_for(lk, std::chrono::milliseconds(poll_ms), [g] { return g->stop; })) return;
        }
        ULONG need = 0;
        LONG st = nt_query(kSystemProcessInformation, buf.data(), static_cast<ULONG>(buf.size()), &need);
        if (st < 0) {
            if (need > buf.size()) buf.resize(need + (need >> 3));
            continue;
        }
        const ThreadRec* mine = nullptr;
        ULONG nthreads = 0;
        for (size_t off = 0;;) {
            const ProcRec* p = reinterpret_cast<const ProcRec*>(buf.data() + off);
            // UniqueProcessId sits at offset 0x50 of SYSTEM_PROCESS_INFORMATION on x64
            const uint64_t ppid = *reinterpret_cast<const uint64_t*>(buf.data() + off + 0x50);
            if (ppid == pid) {
                mine = reinterpret_cast<const ThreadRec*>(buf.data() + off + sizeof(ProcRec));
                nthreads = p->number_of_threads;
                break;
            }
            if (!p->next_entry_offset) break;
            off += p->next_entry_offset;
        }
        if (!mine) continue;
        std::unordered_map<uint32_t, const ThreadRec*> by_tid;
        for (ULONG i = 0; i < nthreads; i++) by_tid[static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mine[i].unique_thread))] = &mine[i];
        for (const std::shared_ptr<KThread>& t : threads()) {
            if (!t->started || t->exited || !t->host_handle || !t->host_tid) continue;
            auto it = by_tid.find(t->host_tid);
            Seen& s = seen[t->host_tid];
            if (it == by_tid.end()) {
                s = Seen{};
                continue;
            }
            const ThreadRec& r = *it->second;
            const bool ready = r.thread_state == kStateReady || r.thread_state == kStateStandby;
            if (ready && r.context_switches == s.switches) s.ready_polls++;
            else s.ready_polls = ready ? 1 : 0;
            s.switches = r.context_switches;
            HANDLE h = static_cast<HANDLE>(t->host_handle);
            if (s.boosted) {                                    // boosted: keep it for boost_polls polls, then back to its band
                if (++s.boost_polls >= kBoostPolls) {
                    SetThreadPriority(h, sched_host_priority(t->prio));
                    t->aged = false;
                    s.boosted = false;
                    s.ready_polls = 0;
                }
            } else if (s.ready_polls >= need_polls) {
                SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
                t->aged = true;                                 // its yields keep the processor while it is aged
                s.boosted = true;
                s.boost_polls = 0;
                const uint64_t n = ++g->boosts;
                if (n <= 5 || (n & (n - 1)) == 0)
                    NXLOG_W("sched", "%s was ready for %d ms without running (the host's processor is busy with other programs): boosted for %d ms (boost %llu; [sched] priority_class = high avoids it)",
                            t->name.c_str(), s.ready_polls * poll_ms, kBoostPolls * poll_ms, static_cast<unsigned long long>(n));
            }
        }
    }
}

}  // namespace

void starvation_guard_start() {
    if (!config_get_bool("sched", "starvation_guard", true)) {
        NXLOG_I("sched", "[sched] starvation_guard = 0: guest threads the host leaves ready are not aged");
        return;
    }
    Guard& g = guard();
    std::lock_guard<std::mutex> lk(g.m);
    if (g.th.joinable()) return;
    const int ms = static_cast<int>(config_get_int("sched", "starvation_ms", 300));
    const int poll = 150;
    const int need = ms < poll ? 1 : ms / poll;
    g.stop = false;
    g.th = std::thread(loop, &g, poll, need);
    NXLOG_I("sched", "starvation guard on: a guest thread ready for %d ms without running is boosted to top priority for 450 ms (debug note 422)", need * poll);
}

void starvation_guard_stop() {
    Guard& g = guard();
    {
        std::lock_guard<std::mutex> lk(g.m);
        if (!g.th.joinable()) return;
        g.stop = true;
    }
    g.cv.notify_all();
    g.th.join();
}

uint64_t starvation_boosts() { return guard().boosts.load(); }

}  // namespace nxrt::kernel
