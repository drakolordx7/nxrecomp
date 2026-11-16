// SPDX-License-Identifier: GPL-3.0-or-later
// Linux implementation of nxrt/os/thread.h: pthreads, nice values per thread, sched_setaffinity, /sys for the CPU layout.
//
// What Linux cannot match of the Windows behaviour:
//  * Priority bands. The six Windows bands become nice offsets from the process's nice value (kNiceOffset below) in the normal scheduling
//    class; there is no real-time class here (SCHED_FIFO needs privileges). Nice can only be raised numerically (lower priority) without
//    CAP_SYS_NICE or an RLIMIT_NICE above 0: the bands above Normal fail there (thread_place returns false and the thread keeps the nice
//    it has; with an RLIMIT_NICE the nice is clamped to what the limit allows), and a thread cannot go back to a higher band after it was
//    lowered. A new thread starts with the nice of the thread that created it. Adjacent bands differ by about a factor 1.5 to 2 in CPU
//    share under contention, not by strict preemption as on Windows; a ready thread of a lower band still runs.
//  * Dynamic priority boost: there is none to switch off (the placement flag is ignored).
//  * Thread suspension and the system-wide thread states (thread_run_states): not provided.
#include "nxrt/os/thread.h"

#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "futex.h"

namespace nxrt::os {
namespace {

using u32 = uint32_t;

// One per host thread known to this layer: threads thread_start made, and threads that asked for a handle (adopted). Reference counted: one
// reference for the thread itself (dropped by its thread-specific-data destructor, which runs after its thread_local destructors) and one
// for every owned handle.
struct Rec {
    std::atomic<int> refs{1};
    pthread_t th{};
    u32 tid = 0;
    u32 tid_ready = 0;          // futex: set when the thread has published its tid
    u32 go = 0;                 // futex: set when the creator has filled in the handle and tid
    u32 done = 0;               // futex: set when the thread is over
    std::atomic<int> band{0};
    std::atomic<uint64_t> final_cpu_ns{0};
    ThreadFn fn = nullptr;
    void* arg = nullptr;
    ThreadPlacement placement;
    char name[16] = {};
};

thread_local Rec* t_rec = nullptr;
thread_local jmp_buf* t_exit = nullptr;
thread_local u32 t_tid = 0;

pthread_key_t g_key;
pthread_once_t g_key_once = PTHREAD_ONCE_INIT;

void unref(Rec* r) {
    if (r->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete r;
}

uint64_t self_cpu_ns() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// Runs when the thread ends, after its thread_local objects are destroyed.
void rec_dtor(void* p) {
    Rec* r = static_cast<Rec*>(p);
    r->final_cpu_ns.store(self_cpu_ns());
    __atomic_store_n(&r->done, 1u, __ATOMIC_RELEASE);
    posix::futex_wake_all(&r->done);
    unref(r);
}

void make_key() { pthread_key_create(&g_key, rec_dtor); }

u32 gettid_cached() {
    if (!t_tid) t_tid = static_cast<u32>(syscall(SYS_gettid));
    return t_tid;
}

// The record of the calling thread, made on first use for threads thread_start did not create.
Rec* self_rec() {
    if (t_rec) return t_rec;
    pthread_once(&g_key_once, make_key);
    Rec* r = new Rec;
    r->th = pthread_self();
    r->tid = gettid_cached();
    r->tid_ready = r->go = 1;
    t_rec = r;
    pthread_setspecific(g_key, r);
    return r;
}

// ---- bands -------------------------------------------------------------------------------------------------------------------

// Nice offsets from the process's own nice value: above Normal needs privileges, see the top of this file.
int nice_offset(PriorityBand b) {
    switch (b) {
        case PriorityBand::TimeCritical: return -8;
        case PriorityBand::Highest: return -5;
        case PriorityBand::AboveNormal: return -2;
        case PriorityBand::Normal: return 0;
        case PriorityBand::BelowNormal: return 5;
        case PriorityBand::Lowest: return 10;
        case PriorityBand::Idle: return 19;
    }
    return 0;
}

std::atomic<int> g_base_nice{INT_MIN};

int base_nice() {
    int b = g_base_nice.load();
    if (b == INT_MIN) {
        errno = 0;
        b = getpriority(PRIO_PROCESS, 0);
        if (errno) b = 0;
        g_base_nice.store(b);
    }
    return b;
}

bool set_nice(u32 tid, int nice) {
    nice = std::max(-20, std::min(19, nice));
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice) == 0) return true;
    if (errno == EACCES || errno == EPERM) {
        // an RLIMIT_NICE may allow part of the way: the lowest nice it permits is 20 - limit
        rlimit rl{};
        if (getrlimit(RLIMIT_NICE, &rl) == 0 && rl.rlim_cur > 0 && rl.rlim_cur <= 40) {
            const int floor_nice = 20 - static_cast<int>(rl.rlim_cur);
            if (floor_nice > nice && setpriority(PRIO_PROCESS, static_cast<id_t>(tid), floor_nice) == 0) return true;
        }
    }
    return false;
}

bool set_affinity(u32 tid, uint64_t mask) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    return sched_setaffinity(static_cast<pid_t>(tid), sizeof set, &set) == 0;
}

bool place_rec(Rec* r, const ThreadPlacement& p) {
    if (__atomic_load_n(&r->done, __ATOMIC_ACQUIRE)) return false;
    bool ok = true;
    if (p.affinity) ok &= set_affinity(r->tid, p.affinity);
    if (p.set_band) {
        if (set_nice(r->tid, base_nice() + nice_offset(p.band))) r->band.store(static_cast<int>(p.band));
        else ok = false;
    }
    return ok;
}

void set_name_self(const char* name) {
    if (!name || !*name) return;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%s", name);     // the kernel keeps 15 characters
    pthread_setname_np(pthread_self(), buf);
}

void wait_flag(const u32* flag) {
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) posix::futex_wait(flag, 0);
}

void* thread_main(void* p) {
    Rec* r = static_cast<Rec*>(p);
    pthread_once(&g_key_once, make_key);
    t_rec = r;
    r->tid = gettid_cached();
    pthread_setspecific(g_key, r);
    __atomic_store_n(&r->tid_ready, 1u, __ATOMIC_RELEASE);
    posix::futex_wake_all(&r->tid_ready);
    wait_flag(&r->go);                       // the creator publishes the handle and tid first
    place_rec(r, r->placement);
    set_name_self(r->name);
    jmp_buf jb;
    if (_setjmp(jb) == 0) {
        t_exit = &jb;
        r->fn(r->arg);
    }
    t_exit = nullptr;
    return nullptr;
}

struct StackRun {
    int (*fn)(void*);
    void* arg;
    int result;
};

void* stack_run_main(void* p) {
    auto* s = static_cast<StackRun*>(p);
    s->result = s->fn(s->arg);
    return nullptr;
}

}  // namespace

bool thread_start(const ThreadOptions& opt, ThreadFn fn, void* arg, ThreadHandle* handle, ThreadId* tid) {
    Rec* r = new Rec;
    r->refs.store(2);                        // the thread, and the handle returned here
    r->fn = fn;
    r->arg = arg;
    r->placement = opt.placement;
    if (opt.name) std::snprintf(r->name, sizeof r->name, "%s", opt.name);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (opt.stack_bytes) pthread_attr_setstacksize(&attr, std::max<size_t>(opt.stack_bytes, PTHREAD_STACK_MIN));
    const int rc = pthread_create(&r->th, &attr, thread_main, r);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete r;
        return false;
    }
    wait_flag(&r->tid_ready);
    if (handle) *handle = r;
    if (tid) *tid = r->tid;
    __atomic_store_n(&r->go, 1u, __ATOMIC_RELEASE);
    posix::futex_wake_all(&r->go);
    return true;
}

void thread_exit() {
    if (t_exit) _longjmp(*t_exit, 1);
    pthread_exit(nullptr);
}

bool thread_join(ThreadHandle h, uint32_t timeout_ms) {
    if (!h) return true;
    Rec* r = static_cast<Rec*>(h);
    const uint64_t deadline_ns = timeout_ms == kWaitForever ? 0 : static_cast<uint64_t>(timeout_ms) * 1000000ull;
    timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (__atomic_load_n(&r->done, __ATOMIC_ACQUIRE)) return true;
        if (timeout_ms == kWaitForever) {
            posix::futex_wait(&r->done, 0);
            continue;
        }
        timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const uint64_t used = static_cast<uint64_t>(t1.tv_sec - t0.tv_sec) * 1000000000ull + static_cast<uint64_t>(t1.tv_nsec - t0.tv_nsec);
        if (used >= deadline_ns) return false;
        posix::futex_wait(&r->done, 0, static_cast<int64_t>(deadline_ns - used));
    }
}

void thread_close(ThreadHandle h) {
    if (h) unref(static_cast<Rec*>(h));
}

ThreadHandle thread_dup(ThreadHandle h) {
    if (!h) return nullptr;
    static_cast<Rec*>(h)->refs.fetch_add(1);
    return h;
}

ThreadHandle thread_adopt_current() {
    Rec* r = self_rec();
    r->refs.fetch_add(1);
    return r;
}

ThreadHandle thread_current() { return self_rec(); }
ThreadId thread_current_id() { return gettid_cached(); }
ThreadId thread_id(ThreadHandle h) { return h ? static_cast<Rec*>(h)->tid : 0; }

bool thread_place(ThreadHandle h, const ThreadPlacement& p) { return h && place_rec(static_cast<Rec*>(h), p); }

bool thread_set_band(ThreadHandle h, PriorityBand band) {
    ThreadPlacement p;
    p.set_band = true;
    p.band = band;
    return thread_place(h, p);
}

bool thread_set_efficiency(ThreadHandle, bool) { return false; }

PriorityBand thread_get_band(ThreadHandle h) { return h ? static_cast<PriorityBand>(static_cast<Rec*>(h)->band.load()) : PriorityBand::Normal; }

uint64_t thread_get_affinity(ThreadHandle h) {
    if (!h) return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(static_cast<pid_t>(static_cast<Rec*>(h)->tid), sizeof set, &set) != 0) return 0;
    uint64_t m = 0;
    for (int i = 0; i < 64; i++)
        if (CPU_ISSET(i, &set)) m |= 1ull << i;
    return m;
}

void thread_set_name(const char* name) { set_name_self(name); }

void thread_yield() { sched_yield(); }
void cpu_relax() { posix::relax(); }

int cpu_current() {
    const int c = sched_getcpu();
    return c >= 0 && c < 64 ? c : -1;
}

uint64_t thread_cpu_time_ns(ThreadHandle h) {
    if (!h) return 0;
    Rec* r = static_cast<Rec*>(h);
    if (__atomic_load_n(&r->done, __ATOMIC_ACQUIRE)) return r->final_cpu_ns.load();
    // the kernel's per-thread CPU clock of a tid (what pthread_getcpuclockid builds): CPUCLOCK_SCHED | CPUCLOCK_PERTHREAD_MASK
    const clockid_t id = static_cast<clockid_t>((~static_cast<uint32_t>(r->tid)) << 3) | 6;
    timespec ts;
    if (clock_gettime(id, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

int run_with_stack(size_t stack_bytes, int (*fn)(void*), void* arg) {
    StackRun run{fn, arg, 0};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, std::max<size_t>(stack_bytes, PTHREAD_STACK_MIN));
    pthread_t th;
    const int rc = pthread_create(&th, &attr, stack_run_main, &run);
    pthread_attr_destroy(&attr);
    if (rc != 0) return fn(arg);
    pthread_join(th, nullptr);
    return run.result;
}

// ---- CPU layout and process scheduling ----------------------------------------------------------------------------------------

// Read once: sched_getaffinity(0) reports the calling thread, which a later pin would narrow, so the answer is the mask the process started with.
uint64_t process_cpu_mask() {
    static const uint64_t mask = [] {
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof set, &set) != 0) return uint64_t{0};
        uint64_t m = 0;
        for (int i = 0; i < 64; i++)
            if (CPU_ISSET(i, &set)) m |= 1ull << i;
        return m;
    }();
    return mask;
}

namespace {

bool read_file(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[256];
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    out = buf;
    return true;
}

// "0-1,8,10-11" -> mask of the first 64 CPUs
uint64_t parse_cpu_list(const std::string& s) {
    uint64_t m = 0;
    const char* p = s.c_str();
    while (*p) {
        char* end = nullptr;
        const long a = std::strtol(p, &end, 10);
        if (end == p) break;
        long b = a;
        p = end;
        if (*p == '-') {
            b = std::strtol(p + 1, &end, 10);
            p = end;
        }
        for (long i = a; i <= b && i < 64; i++)
            if (i >= 0) m |= 1ull << i;
        while (*p == ',' || *p == ' ' || *p == '\n') p++;
    }
    return m;
}

}  // namespace

// Cores come from the sibling lists under /sys/devices/system/cpu. The speed of a core is its cpu_capacity (Arm, some hybrid kernels) or its
// maximum frequency; a core within 15 percent of the fastest one counts as fast. This is a heuristic: Linux has no EfficiencyClass.
std::vector<uint64_t> cores_fastest_first(uint64_t allowed) {
    struct C {
        int fast;
        uint64_t first;
    };
    std::vector<C> cores;
    std::vector<uint64_t> seen_groups;
    std::vector<long> speeds;
    std::vector<uint64_t> firsts;
    for (int cpu = 0; cpu < 64; cpu++) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
        std::string sib;
        if (!read_file(path, sib)) continue;
        const uint64_t group = parse_cpu_list(sib);
        if (!group || std::find(seen_groups.begin(), seen_groups.end(), group) != seen_groups.end()) continue;
        seen_groups.push_back(group);
        const uint64_t usable = group & allowed;
        if (!usable) continue;
        long speed = 0;
        std::string v;
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
        if (!read_file(path, v)) {
            std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
            if (!read_file(path, v)) v.clear();
        }
        if (!v.empty()) speed = std::strtol(v.c_str(), nullptr, 10);
        speeds.push_back(speed);
        firsts.push_back(usable & (~usable + 1));
    }
    if (firsts.empty()) return {};
    const long top = *std::max_element(speeds.begin(), speeds.end());
    for (size_t i = 0; i < firsts.size(); i++) cores.push_back({top > 0 && speeds[i] * 100 >= top * 85 ? 1 : 0, firsts[i]});
    std::stable_sort(cores.begin(), cores.end(), [](const C& a, const C& b) { return a.fast > b.fast || (a.fast == b.fast && a.first < b.first); });
    std::vector<uint64_t> out;
    for (const C& c : cores) out.push_back(c.first);
    return out;
}

bool process_set_priority(ProcessPriority p) {
    const int nice = p == ProcessPriority::High ? -5 : p == ProcessPriority::AboveNormal ? -2 : 0;
    if (!set_nice(gettid_cached(), nice)) return false;
    g_base_nice.store(nice);
    return true;
}

SlotResult slot_claim(const char* name, void** token) {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) dir = "/tmp";
    const std::string path = std::string(dir) + "/" + name + ".lock";
    const int fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return SlotResult::Error;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        close(fd);
        return e == EWOULDBLOCK ? SlotResult::Busy : SlotResult::Error;
    }
    *token = reinterpret_cast<void*>(static_cast<intptr_t>(fd) + 1);
    return SlotResult::Claimed;
}

void slot_release(void* token) {
    if (token) close(static_cast<int>(reinterpret_cast<intptr_t>(token) - 1));
}

bool thread_run_states(std::vector<ThreadRunState>& out) {
    out.clear();
    return false;
}

bool thread_run_states_supported() { return false; }

void thread_boost_audio() { thread_set_band(thread_current(), PriorityBand::Highest); }

size_t process_handle_count() {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return 0;
    size_t n = 0;
    while (dirent* e = readdir(d))
        if (e->d_name[0] != '.') n++;
    closedir(d);
    return n > 0 ? n - 1 : 0;      // the descriptor of the directory itself
}

}  // namespace nxrt::os
