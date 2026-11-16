# nxrt/os: the platform layer

The one place where the runtime talks to the host operating system. Headers here describe a small, platform-neutral interface; the code behind
them lives in `src/os/win32/` (Windows) and `src/os/posix/` (Linux) and CMake builds the folder of the platform it targets
(`cmake/nx_platform.cmake`, a glob: a new `.cpp` in either folder needs no CMake edit).

Rule: no `#ifdef _WIN32`, no `<windows.h>` and no other OS header outside `src/os/` (and the headers in this folder). Everything else calls
`nxrt/os/*.h`. A new OS call is added here once, with one implementation per platform, so a fix applies to both.

The modules (each one a header here and one source file per platform; `fs.h`, `process.h` and `net.h` exist, the rest are planned):

| module | what |
| --- | --- |
| `mem` | reserve/commit/protect/free of address ranges at fixed addresses, page size, write watch |
| `fault` | access-violation / SIGSEGV hooks, guest traps, termination of the process |
| `thread` | host threads (stack size, priority, affinity, names), TLS, suspend/capture for the watchdog |
| `sync` | host mutexes, events, futex-style waits |
| `time` | monotonic clock, high-resolution sleep and wait, wall clock |
| `process` | (done) pid, exe path and folder, environment, user/portable folders, language, random bytes, locks between processes; planned: child processes, command line |
| `fs` | (done) UTF-8 paths, positioned file I/O, stat, folder listing, durable rename, case-insensitive path resolver; planned: memory-mapped files |
| `net` | (done) IPv4 sockets, poll, options; errors come back as Linux errno numbers |
| `dynlib` | loading shared libraries and finding symbols in them |
| `symbols` | host address to name (map file or ELF symbols), stack capture |

`src/os/sync.cpp` (the guest-address wait objects of the kernel) is not part of this layer and stays where it is.
