# nxrt/os: the platform layer

The one place where the runtime talks to the host operating system. Headers here describe a small, platform-neutral interface; the code behind
them lives in `src/os/win32/` (Windows) and `src/os/posix/` (Linux) and CMake builds the folder of the platform it targets
(`cmake/nx_platform.cmake`, a glob: a new `.cpp` in either folder needs no CMake edit).

Rule: no `#ifdef _WIN32`, no `<windows.h>` and no other OS header outside `src/os/` (and the headers in this folder). Everything else calls
`nxrt/os/*.h`. A new OS call is added here once, with one implementation per platform, so a fix applies to both.

Modules (each one a header here and one source file per platform; those marked done exist):

| module | what |
| --- | --- |
| `mem` (done) | reserve/commit/protect/free of address ranges at fixed addresses, page size, write watch |
| `fault` (done) | access-violation / SIGSEGV hooks, guest traps, termination of the process |
| `thread` (done) | host threads (stack size, priority, affinity, names), TLS, suspend/capture for the watchdog |
| `sync` (done) | host mutexes, events, futex-style waits |
| `time` (done) | monotonic clock, high-resolution sleep and wait, wall clock |
| `process` | (done) pid, exe path and folder, environment, user/portable folders, language, random bytes, locks between processes; planned: child processes, command line |
| `fs` | (done) UTF-8 paths, positioned file I/O, stat, folder listing, durable rename, case-insensitive path resolver; planned: memory-mapped files |
| `net` | (done) IPv4 sockets, poll, options; errors come back as Linux errno numbers |
| `dynlib` | loading shared libraries and finding symbols in them (not needed so far: no caller loads a library by name) |
| `symbols` | (done) host address to name (Windows linker map, Linux ELF symtab + dladdr), loaded modules, stack capture (RtlVirtualUnwind, libgcc unwinder) |
| `procinfo` | (done) memory, CPU time, handle and thread counts of this process (the soak sampler) |
| `hwwatch` | (done on Windows only) data breakpoints on the process's own memory, for developer probes; Linux says unsupported |

`movie_mf.cpp` (`win32`: the Media Foundation decoder of the `movie::` library, a fallback behind `[media] backend = mf`; `posix`: a stub) is the one
platform file that is not an OS module: the FFmpeg backend is the same code on both systems (`src/media/movie_ffmpeg.cpp`).

`src/os/sync.cpp` (the guest-address wait objects of the kernel) is not part of this layer and stays where it is.

## thread, sync, time

- `thread.h`: `thread_start` (stack, name, priority band and CPU mask applied before the thread runs), `thread_join`, handles (`thread_close`, `thread_dup`, `thread_adopt_current`, `thread_current`), `PriorityBand` (bands, Windows THREAD_PRIORITY numbers), `thread_place`, `thread_exit`, `run_with_stack` (the guest main thread needs 128 MiB: Windows links it into the exe, Linux runs on a pthread), the CPU layout (`process_cpu_mask`, `cores_fastest_first`), `process_set_priority`, `slot_claim` (cross-process core slots).
- `sync.h`: `Mutex`, `SharedMutex`, `CondVar`, constant-initialised, usable with `std::lock_guard` and friends (SRWLOCK and CONDITION_VARIABLE on Windows, futexes on Linux).
- `time.h`: `monotonic_ns`, `tick_ms`, `wall_filetime`, `sleep_precise_ns` / `sleep_ns` / `sleep_ms` / `wait_until_ns`, `timer_resolution_acquire` / `release`.
- Linux priority bands are nice offsets and cannot go above the process's nice value without privileges: see the top of `src/os/posix/thread.cpp`.

## diagnostics

- `symbols.h`: `lookup` (module + nearest symbol), `load` / `prepare` (the table of the main exe: Windows reads `<exe>.map`, Linux reads `/proc/self/exe`, which must not be stripped), `modules`, `capture_current` and `capture_fault` (host stack from inside a fault handler). Linux stacks come from libgcc's unwinder over `.eh_frame`, so neither the runtime nor the lifted code needs frame pointers.
- `thread.h`: `thread_sample` stops a thread for a moment and walks its stack (the freeze watchdog and the sampling profiler). Windows: SuspendThread. Linux: a real-time signal (`SIGRTMIN+3`) with a handler on the sampled thread; sleeps that never restart (`poll`, `epoll_wait`) may return `EINTR` once, and a thread that does not answer in 200 ms counts as "could not stop". `process_thread_count` sits beside `process_handle_count`.
- `fault.h`: the crash reporter (`src/diag/crash.cpp`) is the unhandled handler. On Linux SIGILL, SIGFPE and SIGABRT reach it as kind Other once it is installed; a Linux report maps the signal to the nearest Windows exception code and adds a "signal" line. No minidump: Windows keeps what the OS gives, Linux hands the fault back to the system after the report when `[diag] crash_core` is set (a core file where `ulimit -c` allows).
