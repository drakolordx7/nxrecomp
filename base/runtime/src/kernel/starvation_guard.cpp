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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/os/thread.h"

namespace nxrt::kernel {
namespace {

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
    uint32_t switches = 0;
    int ready_polls = 0;
    int boost_polls = 0;
    bool boosted = false;
};
constexpr int kBoostPolls = 3;

void loop(Guard* g, int poll_ms, int need_polls) {
    log_set_thread_name("starve-guard");
    os::thread_set_band(os::thread_current(), os::PriorityBand::Highest);        // it must run when everything else is starved
    std::vector<os::ThreadRunState> states;
    std::unordered_map<uint32_t, const os::ThreadRunState*> by_tid;
    std::unordered_map<uint32_t, Seen> seen;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g->m);
            if (g->cv.wait_for(lk, std::chrono::milliseconds(poll_ms), [g] { return g->stop; })) return;
        }
        if (!os::thread_run_states(states)) continue;
        by_tid.clear();
        for (const os::ThreadRunState& r : states) by_tid[r.tid] = &r;
        for (const std::shared_ptr<KThread>& t : threads()) {
            if (!t->started || t->exited || !t->host_handle || !t->host_tid) continue;
            auto it = by_tid.find(t->host_tid);
            Seen& s = seen[t->host_tid];
            if (it == by_tid.end()) {
                s = Seen{};
                continue;
            }
            const os::ThreadRunState& r = *it->second;
            const bool ready = r.ready;
            if (ready && r.switches == s.switches) s.ready_polls++;
            else s.ready_polls = ready ? 1 : 0;
            s.switches = r.switches;
            os::ThreadHandle h = t->host_handle;
            if (s.boosted) {
                if (++s.boost_polls >= kBoostPolls) {
                    os::thread_set_band(h, sched_host_priority(t->prio));
                    t->aged = false;
                    s.boosted = false;
                    s.ready_polls = 0;
                }
            } else if (s.ready_polls >= need_polls) {
                os::thread_set_band(h, os::PriorityBand::TimeCritical);
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

}

void starvation_guard_start() {
    if (!config_get_bool("sched", "starvation_guard", true)) {
        NXLOG_I("sched", "[sched] starvation_guard = 0: guest threads the host leaves ready are not aged");
        return;
    }
    if (!os::thread_run_states_supported()) {
        NXLOG_I("sched", "starvation guard not started: this host does not report thread states (its scheduler does not starve a ready thread)");
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

namespace {
const bool g_gauge_boosts_registered = [] {
    nxrt::gauge_register("kernel.starvation_boosts", [] { return static_cast<int64_t>(starvation_boosts()); });
    return true;
}();
}

}
