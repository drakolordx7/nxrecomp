// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os thread: host threads, their priority and CPU placement, and the host's CPU layout. Windows: CreateThread, SetThreadPriority,
// SetThreadAffinityMask, GetLogicalProcessorInformationEx. Linux: pthreads, setpriority (nice) per thread, sched_setaffinity, /sys.
// See README.md in this folder for the rules of the layer, and src/os/posix/thread.cpp for what Linux cannot match.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nxrt::os {

// Opaque handle to a host thread; nullptr is none. Windows: the thread HANDLE. Linux: a reference-counted record. An owned handle (from
// thread_start, thread_adopt_current, thread_dup) is released with thread_close.
using ThreadHandle = void*;
using ThreadId = uint32_t;                       // Windows: the thread id; Linux: the kernel tid
constexpr uint32_t kWaitForever = 0xFFFFFFFFu;   // the timeout of thread_join that never ends

// Six priority bands inside the normal scheduling class. The numbers are the Windows THREAD_PRIORITY_* values. Linux maps a band to a nice
// offset (src/os/posix/thread.cpp).
enum class PriorityBand : int {
    Lowest = -2,
    BelowNormal = -1,
    Normal = 0,
    AboveNormal = 1,
    Highest = 2,
    TimeCritical = 15,
};

// Where and how a thread runs.
struct ThreadPlacement {
    uint64_t affinity = 0;           // host CPU mask (bit n = logical CPU n of the first 64); 0 = leave as it is
    bool set_band = false;           // apply `band`
    PriorityBand band = PriorityBand::Normal;
    bool disable_boost = false;      // Windows: dynamic priority boost off. Linux: nothing to switch off
};

using ThreadFn = void (*)(void*);

struct ThreadOptions {
    size_t stack_bytes = 0;          // 0 = the host default; the address space is reserved, not committed (Windows STACK_SIZE_PARAM_IS_A_RESERVATION)
    const char* name = nullptr;      // shown by debuggers (Linux keeps 15 characters)
    ThreadPlacement placement;       // applied before fn runs
};

// Starts a thread that runs fn(arg) and ends when fn returns (or thread_exit is called on it). `*handle` and `*tid` are filled in before fn
// starts to run, so the new thread can read them from state the caller set up. False on failure (nothing started, nothing to close).
bool thread_start(const ThreadOptions& opt, ThreadFn fn, void* arg, ThreadHandle* handle, ThreadId* tid);

// Ends the calling thread now (Windows: ExitThread; Linux: a jump back to the thread's entry). No destructors of the skipped frames run on
// either system, so the caller must hold no object that needs one. On a thread that thread_start did not create, Linux uses pthread_exit.
[[noreturn]] void thread_exit();

// Waits until the thread has finished, at most timeout_ms (kWaitForever = no limit). True when it has finished. The thread's thread_local
// destructors have run by then.
bool thread_join(ThreadHandle h, uint32_t timeout_ms);

void thread_close(ThreadHandle h);               // releases an owned handle; the thread itself is not touched
ThreadHandle thread_dup(ThreadHandle h);         // another owned handle to the same thread (nullptr on failure)
ThreadHandle thread_adopt_current();             // an owned handle to the calling thread, for a thread thread_start did not create (nullptr on failure)
ThreadHandle thread_current();                   // the calling thread's handle: borrowed, valid on this thread only, never closed
ThreadId thread_current_id();
ThreadId thread_id(ThreadHandle h);              // 0 on failure

// Priority and CPU placement of any thread, running or not. False when the host refused; the parts that were possible still apply.
bool thread_place(ThreadHandle h, const ThreadPlacement& p);
bool thread_set_band(ThreadHandle h, PriorityBand band);
// The band last applied to the thread through this layer; Windows asks the system. Normal for a thread never given one.
PriorityBand thread_get_band(ThreadHandle h);
uint64_t thread_get_affinity(ThreadHandle h);    // host CPU mask of the thread; 0 when it cannot be read or CPUs beyond the first 64 are involved
void thread_set_name(const char* name);          // names the calling thread for debuggers (Linux keeps 15 characters)

// Offers the processor to another ready thread (SwitchToThread, sched_yield). cpu_relax is the spin-loop hint (PAUSE) and gives nothing away.
void thread_yield();
void cpu_relax();
int cpu_current();                               // host CPU the calling thread runs on right now (first 64), -1 when unknown

// CPU time the thread has used (kernel plus user), nanoseconds; 0 on failure.
uint64_t thread_cpu_time_ns(ThreadHandle h);

// Runs fn(arg) on a thread with at least `stack_bytes` of stack and returns its result. Windows: on the calling thread (the exe is linked
// with the big stack). Linux: on a new pthread, because the main thread's stack is fixed at 8 MiB; the caller waits for it.
int run_with_stack(size_t stack_bytes, int (*fn)(void*), void* arg);

// ---- the CPU layout and process scheduling ------------------------------------------------------------------------------------

// The CPUs the process may use, as a host CPU mask (the first 64).
uint64_t process_cpu_mask();

// The physical cores inside `allowed`, fastest first (a hybrid CPU's performance cores before its efficiency cores; ties in CPU order),
// each as the mask of ONE of its hardware threads (the lowest allowed). Empty when the host cannot tell. Linux reads /sys/devices/system/cpu.
std::vector<uint64_t> cores_fastest_first(uint64_t allowed);

enum class ProcessPriority { Normal, AboveNormal, High };
// Raises the process priority (Windows: the priority class; Linux: the nice value of the calling thread, which the threads created after
// this call inherit, so call it early; it needs CAP_SYS_NICE or RLIMIT_NICE). False when refused.
bool process_set_priority(ProcessPriority p);

// A named claim between processes of this program, held until released or the process ends (Windows: a named mutex in the Local namespace;
// Linux: flock on a file in $XDG_RUNTIME_DIR or /tmp). Used to keep two running copies off the same cores.
enum class SlotResult { Claimed, Busy, Error };
SlotResult slot_claim(const char* name, void** token);
void slot_release(void* token);

// ---- hosts that can report what their threads are doing -----------------------------------------------------------------------

struct ThreadRunState {
    ThreadId tid;
    bool ready;                // runnable but not on a CPU
    uint32_t switches;         // context switches so far
};
// The state of this process's threads. False where the host cannot say (Linux: the fair scheduler leaves nothing ready for long) or the
// call failed. Meant for one caller on one thread (the starvation guard); it keeps a buffer between calls.
bool thread_run_states(std::vector<ThreadRunState>& out);
bool thread_run_states_supported();              // whether thread_run_states can work on this host at all

// ---- audio ---------------------------------------------------------------------------------------------------------------------

// Asks the host to schedule the calling (audio) thread for timely delivery (Windows: MMCSS task "Pro Audio", else the Highest band; Linux:
// the Highest band where the nice value allows). Undone when the thread ends.
void thread_boost_audio();

// ---- tests and leak checks ---------------------------------------------------------------------------------------------------

// Open OS handles (Windows) or file descriptors (Linux) of the process, 0 when unknown.
size_t process_handle_count();

}  // namespace nxrt::os
