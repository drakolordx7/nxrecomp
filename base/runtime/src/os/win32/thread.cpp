// SPDX-License-Identifier: GPL-3.0-or-later
// Windows implementation of nxrt/os/thread.h. The code moved here from kernel/thread.cpp, kernel/sched.cpp, kernel/starvation_guard.cpp and
// services/audren/Integration/ThreadBoost.h; behaviour is unchanged.
#include "nxrt/os/thread.h"

#include "nxrt/os/fault.h"
#include "walk.h"

#include <windows.h>
#include <avrt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#pragma comment(lib, "avrt.lib")

namespace nxrt::os {
namespace {

HANDLE H(ThreadHandle h) { return static_cast<HANDLE>(h); }

struct Start {
    ThreadFn fn;
    void* arg;
};

DWORD WINAPI thread_main(LPVOID p) {
    Start s = *static_cast<Start*>(p);
    delete static_cast<Start*>(p);
    s.fn(s.arg);
    return 0;
}

void set_name_of(HANDLE h, const char* name) {
    if (!name || !*name) return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (n <= 0) return;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w.data(), n);
    SetThreadDescription(h, w.c_str());
}

}  // namespace

bool thread_start(const ThreadOptions& opt, ThreadFn fn, void* arg, ThreadHandle* handle, ThreadId* tid) {
    auto* box = new Start{fn, arg};
    DWORD id = 0;
    HANDLE h = CreateThread(nullptr, static_cast<SIZE_T>(opt.stack_bytes), thread_main, box, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &id);
    if (!h) {
        delete box;
        return false;
    }
    thread_place(h, opt.placement);
    set_name_of(h, opt.name);
    if (handle) *handle = h;
    if (tid) *tid = id;
    ResumeThread(h);
    return true;
}

void thread_exit() {
    ExitThread(0);
    for (;;) {}
}

bool thread_join(ThreadHandle h, uint32_t timeout_ms) {
    if (!h) return true;
    return WaitForSingleObject(H(h), timeout_ms) == WAIT_OBJECT_0;
}

void thread_close(ThreadHandle h) {
    if (h) CloseHandle(H(h));
}

ThreadHandle thread_dup(ThreadHandle h) {
    HANDLE d = nullptr;
    if (!h || !DuplicateHandle(GetCurrentProcess(), H(h), GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS)) return nullptr;
    return d;
}

ThreadHandle thread_adopt_current() {
    HANDLE real = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real, THREAD_ALL_ACCESS, FALSE, 0)) return nullptr;
    return real;
}

ThreadHandle thread_current() { return GetCurrentThread(); }
ThreadId thread_current_id() { return GetCurrentThreadId(); }
ThreadId thread_id(ThreadHandle h) { return h ? GetThreadId(H(h)) : 0; }

bool thread_place(ThreadHandle h, const ThreadPlacement& p) {
    if (!h) return false;
    bool ok = true;
    if (p.affinity) ok &= SetThreadAffinityMask(H(h), static_cast<DWORD_PTR>(p.affinity)) != 0;
    if (p.disable_boost) ok &= SetThreadPriorityBoost(H(h), TRUE) != 0;
    if (p.set_band) ok &= SetThreadPriority(H(h), static_cast<int>(p.band)) != 0;
    return ok;
}

bool thread_set_band(ThreadHandle h, PriorityBand band) {
    ThreadPlacement p;
    p.set_band = true;
    p.band = band;
    return thread_place(h, p);
}

bool thread_set_efficiency(ThreadHandle h, bool eco) {
    if (!h) return false;
    THREAD_POWER_THROTTLING_STATE st = {};
    st.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = eco ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;   // 0 with the control bit set: explicitly not throttled
    return SetThreadInformation(H(h), ThreadPowerThrottling, &st, sizeof st) != 0;
}

PriorityBand thread_get_band(ThreadHandle h) {
    const int v = GetThreadPriority(H(h));
    return v == THREAD_PRIORITY_ERROR_RETURN ? PriorityBand::Normal : static_cast<PriorityBand>(v);
}

uint64_t thread_get_affinity(ThreadHandle h) {
    GROUP_AFFINITY ga{};
    if (!h || !GetThreadGroupAffinity(H(h), &ga) || ga.Group != 0) return 0;
    return static_cast<uint64_t>(ga.Mask);
}

void thread_set_name(const char* name) { set_name_of(GetCurrentThread(), name); }

void thread_yield() { SwitchToThread(); }
void cpu_relax() { YieldProcessor(); }

int cpu_current() {
    PROCESSOR_NUMBER pn{};
    GetCurrentProcessorNumberEx(&pn);
    return pn.Group == 0 ? static_cast<int>(pn.Number) : -1;
}

uint64_t thread_cpu_time_ns(ThreadHandle h) {
    if (!h) return 0;
    FILETIME created{}, exited{}, kernel_time{}, user_time{};
    if (!GetThreadTimes(H(h), &created, &exited, &kernel_time, &user_time)) return 0;
    auto to_u64 = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (to_u64(kernel_time) + to_u64(user_time)) * 100;
}

int run_with_stack(size_t, int (*fn)(void*), void* arg) { return fn(arg); }

uint64_t process_cpu_mask() {
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    return static_cast<uint64_t>(proc);
}

// The physical cores of processor group 0 inside `allowed`, fastest first (a hybrid CPU reports a higher EfficiencyClass for its performance
// cores), each as the mask of ONE hardware thread of the core (its lowest).
std::vector<uint64_t> cores_fastest_first(uint64_t allowed) {
    std::vector<uint64_t> out;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len) return out;
    std::vector<uint8_t> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) return out;
    struct C {
        int eff;
        uint64_t first;
    };
    std::vector<C> cores;
    for (DWORD off = 0; off < len;) {
        auto* e = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
        if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount >= 1 && e->Processor.GroupMask[0].Group == 0) {
            const uint64_t m = static_cast<uint64_t>(e->Processor.GroupMask[0].Mask) & allowed;
            if (m) cores.push_back({e->Processor.EfficiencyClass, m & (~m + 1)});
        }
        off += e->Size;
    }
    std::stable_sort(cores.begin(), cores.end(), [](const C& a, const C& b) { return a.eff > b.eff || (a.eff == b.eff && a.first < b.first); });
    for (const C& c : cores) out.push_back(c.first);
    return out;
}

bool process_set_priority(ProcessPriority p) {
    const DWORD cls = p == ProcessPriority::High ? HIGH_PRIORITY_CLASS : p == ProcessPriority::AboveNormal ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
    return SetPriorityClass(GetCurrentProcess(), cls) != 0;
}

SlotResult slot_claim(const char* name, void** token) {
    const std::string full = std::string("Local\\") + name;
    HANDLE m = CreateMutexA(nullptr, FALSE, full.c_str());
    if (!m) return SlotResult::Error;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        return SlotResult::Busy;
    }
    *token = m;
    return SlotResult::Claimed;
}

void slot_release(void* token) {
    if (token) CloseHandle(static_cast<HANDLE>(token));
}

// ---- thread states (NtQuerySystemInformation) ---------------------------------------------------------------------------------

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
    uint8_t body[0x100 - 8];
};
static_assert(sizeof(ProcRec) == 0x100, "SYSTEM_PROCESS_INFORMATION");
constexpr ULONG kStateReady = 1;
constexpr ULONG kStateStandby = 3;
constexpr int kSystemProcessInformation = 5;

using NtQuery = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

}  // namespace

bool thread_run_states(std::vector<ThreadRunState>& out) {
    out.clear();
    static NtQuery nt_query = reinterpret_cast<NtQuery>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation"));
    if (!nt_query) return false;
    static std::vector<uint8_t> buf(1 << 20);
    ULONG need = 0;
    const LONG st = nt_query(kSystemProcessInformation, buf.data(), static_cast<ULONG>(buf.size()), &need);
    if (st < 0) {
        if (need > buf.size()) buf.resize(need + (need >> 3));
        return false;
    }
    const uint32_t pid = GetCurrentProcessId();
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
    if (!mine) return false;
    for (ULONG i = 0; i < nthreads; i++) {
        const bool ready = mine[i].thread_state == kStateReady || mine[i].thread_state == kStateStandby;
        out.push_back({static_cast<ThreadId>(reinterpret_cast<uintptr_t>(mine[i].unique_thread)), ready, mine[i].context_switches});
    }
    return true;
}

bool thread_run_states_supported() { return true; }

void thread_boost_audio() {
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (mm != nullptr) {
        AvSetMmThreadPriority(mm, AVRT_PRIORITY_HIGH);
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
}

SampleResult thread_sample(ThreadHandle h, ThreadSample* out, uint64_t* frames, size_t max_frames) {
    *out = ThreadSample{};
    if (!h || SuspendThread(H(h)) == static_cast<DWORD>(-1)) return SampleResult::NoSuspend;
    CONTEXT tc{};
    tc.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    const bool got = GetThreadContext(H(h), &tc) != 0;
    if (got) {
        out->pc = tc.Rip;
        out->sp = tc.Rsp;
        out->fp = tc.Rbp;
        out->nframes = detail::capture_context(&tc, frames, max_frames);   // while the thread is stopped: its stack cannot change under the walk
    }
    ResumeThread(H(h));
    if (!got) return SampleResult::NoContext;
    uint8_t b[2] = {0, 0};
    out->in_syscall = out->pc >= 2 && guard::read(reinterpret_cast<const void*>(out->pc - 2), b, 2) && b[0] == 0x0F && b[1] == 0x05;
    return SampleResult::Ok;
}

size_t process_thread_count() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    size_t n = 0;
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) do { if (te.th32OwnerProcessID == pid) n++; } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    return n;
}

size_t process_handle_count() {
    DWORD n = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &n) ? static_cast<size_t>(n) : 0;
}

}  // namespace nxrt::os
