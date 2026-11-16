# nxrt/os: the platform layer

The one place where the runtime talks to the host operating system. Headers here describe a small, platform-neutral interface; the code behind
them lives in `src/os/win32/` (Windows) and `src/os/posix/` (Linux) and CMake builds the folder of the platform it targets
(`cmake/nx_platform.cmake`, a glob: a new `.cpp` in either folder needs no CMake edit).

Rule: no `#ifdef _WIN32`, no `<windows.h>` and no other OS header outside `src/os/` (and the headers in this folder). Everything else calls
`nxrt/os/*.h`. A new OS call is added here once, with one implementation per platform, so a fix applies to both.

Modules (each one a header here and one source file per platform; those marked done exist):

| module | what |
| --- | --- |
| `mem` | reserve/commit/protect/free of address ranges at fixed addresses, page size, write watch |
| `fault` | access-violation / SIGSEGV hooks, guest traps, termination of the process |
| `thread` (done) | host threads (stack size, priority, affinity, names), TLS, suspend/capture for the watchdog |
| `sync` (done) | host mutexes, events, futex-style waits |
| `time` (done) | monotonic clock, high-resolution sleep and wait, wall clock |
| `process` | environment, exe path, child processes, command line |
| `fs` | paths and folders (save, config, cache locations), file handles, memory-mapped files |
| `net` | sockets and their error codes |
| `dynlib` | loading shared libraries and finding symbols in them |
| `symbols` | host address to name (map file or ELF symbols), stack capture |

`src/os/sync.cpp` (the guest-address wait objects of the kernel) is not part of this layer and stays where it is.

## thread, sync, time

- `thread.h`: `thread_start` (stack, name, priority band and CPU mask applied before the thread runs), `thread_join`, handles (`thread_close`, `thread_dup`, `thread_adopt_current`, `thread_current`), `PriorityBand` (six bands, Windows THREAD_PRIORITY numbers), `thread_place`, `thread_exit`, `run_with_stack` (the guest main thread needs 128 MiB: Windows links it into the exe, Linux runs on a pthread), the CPU layout (`process_cpu_mask`, `cores_fastest_first`), `process_set_priority`, `slot_claim` (cross-process core slots).
- `sync.h`: `Mutex`, `SharedMutex`, `CondVar`, constant-initialised, usable with `std::lock_guard` and friends (SRWLOCK and CONDITION_VARIABLE on Windows, futexes on Linux).
- `time.h`: `monotonic_ns`, `tick_ms`, `wall_filetime`, `sleep_precise_ns` / `sleep_ns` / `sleep_ms` / `wait_until_ns`, `timer_resolution_acquire` / `release`.
- Linux priority bands are nice offsets and cannot go above the process's nice value without privileges: see the top of `src/os/posix/thread.cpp`.
