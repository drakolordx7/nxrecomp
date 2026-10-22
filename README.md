# nxrt

nxrt is the game-independent OS and HLE runtime for the Metroid Prime Remastered static recompilation. It runs on native Windows x64, is written in C++20 and builds with clang-cl. It is licensed GPL-3.0-or-later. Third-party material is listed in THIRD_PARTY_NOTICES.md.

## Layout

| Folder | Contents |
|---|---|
| `include/nxrt` | 25 public headers: abi, config, diag, dispatch, eh, hipc, hooks, ipc, kernel, log, memguard, memory, module, nxmodule, runtime, svc, symbols, sync, and for the graphics shell frame_limiter, gfx, input, nvn, overlay, platform, scene |
| `src/core` | Bring-up (runtime.cpp), generated-code ABI glue (abi_glue.cpp), lifted-module registry and import resolution (nxmodule.cpp), site hooks (hooks.cpp), indirect dispatch and HLE thunks (dispatch.cpp), ABI version string (build_info.cpp) |
| `src/eh` | C++ exceptions: unwinder (unwind.cpp), Itanium typeinfo matching (typeinfo.cpp), `nx_catch_call` (catch_call.cpp), `_Unwind_*` handlers (handlers.cpp) |
| `src/diag` | Config file and environment lookup (config.cpp), leveled log (log.cpp), crash report (crash.cpp), host stack walk (stackwalk.cpp), linker-map symbols (symbols.cpp), freeze watchdog (watchdog.cpp), memory guard (memguard.cpp), thread registry and recent files (registry.cpp) |
| `src/ipc` | HIPC wire format (hipc.cpp), service registry, sessions, domains and routing (ipc.cpp), `sm:` service manager (sm.cpp), stub services (stub.cpp) |
| `src/kernel` | Handle table (handles.cpp), SVC dispatcher (svc.cpp) and its groups: info (svc_info.cpp), IPC (svc_ipc.cpp), memory (svc_memory.cpp), thread and sync (svc_thread.cpp), names (svc_names.inc); kernel sync semantics (kernel_sync.cpp), guest threads (thread.cpp), console core model (sched.cpp), system tick (time.cpp) |
| `src/loader` | ELF64 AArch64 structures (elf.h), module loader and linker (module.cpp) |
| `src/mem` | Guest address space, heap section and alias views (memory.cpp) |
| `src/os` | Guest-address keyed sync objects (sync.cpp) |
| `src/gfx` | Graphics shell (S4b, library `nxrt_gfx`, see docs/S4b.md): SDL3 platform and input queue (platform.cpp, input.cpp), frame limiter (frame_limiter.cpp), Vulkan device and renderer (vk_device.cpp, vk_renderer.cpp, vma_impl.cpp), test scene and loop (scene.cpp, scene_runner.cpp, shaders/, scene_shaders.inc), ImGui overlay (overlay.cpp), NVN front end (nvn/nvn_loader.cpp, generated nvn/nvn_table.inc and nvn/nvn_sizes.inc) |
| `apps` | `gfx_demo.cpp`: the test scene as an executable (`nxrt_gfx_demo`) |
| `cmake` | `gfx.cmake`, `gfx_deps.cmake`: the graphics library, the demo, the gfx tests and the pinned third-party sources |
| `tests` | 21 unit-test groups (`test_*.cpp`), `test_framework.h`, `test_main.cpp`; `tests/gfx` holds the 8 graphics groups (`nxrt_gfx_tests`), `tests/data/gfx` the reference image |
| `tests/lift` | Integration tests: test programs lifted by the recompiler and run on this runtime (`driver.cpp`, `CMakeLists.txt`), this work's exception programs (`progs`), their miniature libc++abi (`guest/rt2.h`) and its host stand-in (`native`) |
| `tests/data` | Loader fixtures, HIPC test vectors (generated, see Build and test) |
| `tests/fixture` | `fixture.c` and `make_fixture.cmd`, which builds the loader fixtures |
| `tests/tools` | `gen_hipc_vectors.py` (HIPC vectors); for the lifted tests: `guest_build.py`, `make_sites.py`, `unicorn_expect.py`, `native_check.cmd`, `dump_results.py` |
| `docs` | `S3.md`, `S3b.md` and `S4b.md` (stream logs, decisions, gaps; S3b has the C++ exception boundary), `nvn_struct_sizes.md` (NVN object sizes), `CONFIG.md` (every ini key and environment variable), `debug/NNN.md` (numbered bug notes) |
| `scripts` | `build.ps1` (configure, build, optional test), `env.cmd` (MSVC, LLVM, CMake and Ninja on PATH), `gen_nvn_table.py` (NVN table), `gen_scene_shaders.py` (test-scene SPIR-V) |

## Build and test

Requirements: Windows x64, the Visual Studio Build Tools (`env.cmd` calls `vcvars64.bat` from the BuildTools path), LLVM installed in `C:\Program Files\LLVM` (clang-cl; CMake picks `lld-link` from it as the linker), CMake and Ninja. `build.ps1` also passes sccache as the compiler launcher.

    pwsh scripts\build.ps1                      configure and build into build\release
    pwsh scripts\build.ps1 -Test                build, then run CTest
    pwsh scripts\build.ps1 -Test -Filter <re>   run only the CTest entries matching <re>
    pwsh scripts\build.ps1 -Clean               delete build\release first

Output goes to `build\<config in lower case>`, so Release builds to `build\release`.

The ABI header `nxrecomp/ctx.h` is owned by S1. CMake looks for it in `..\recomp\include` relative to the checkout (`NXRT_RECOMP_INCLUDE`, see `CMakeLists.txt`). `include/nxrt/abi.h` asserts `NXRECOMP_ABI_VERSION == 2`. A git worktree does not contain `..\recomp`, so it has to be configured once by hand with `-DNXRT_RECOMP_INCLUDE=C:/mpr/recomp/include`. The CMake cache keeps the value for later `build.ps1` runs.

Test groups: `crash`, `eh`, `eh_typeinfo`, `hipc`, `hooks`, `ipc`, `kernel_sync`, `kernel_threads`, `loader`, `log_config`, `memguard`, `memory`, `nxmodule`, `runtime_e2e`, `smoke`, `svc_info`, `svc_memory`, `svc_names`, `symbols`, `sync`, `watchdog`. Beside them, 25 CTest entries `lift_*` run lifted programs (see Lifted-code tests). CMake reads the `NXTEST_GROUP` line of each test source and creates one CTest entry per group, with a 120 s timeout. To run one group directly, use `build\release\nxrt_tests.exe <group>`. `nxrt_tests.exe --list` prints the groups, `--all` runs everything and `--child <name>` runs a child-process mode (for example `crash_av`, `crash_trap`, `crash_stackoverflow`). The graphics shell has its own 8 groups in `nxrt_gfx_tests` (see docs/S4b.md).

Snapshot on 2026-10-21 (clean Release build): CTest reported 46 of 46 entries passed (a busy machine can push the older timing tests `kernel_sync` and `kernel_threads` over their limits; rerun them alone). `build\release\nxrt_tests.exe --all` printed:

    SUMMARY --all: 314 passed, 0 failed, 0 skipped (of 314)

### Lifted-code tests

`tests/lift` builds each test program with clang for AArch64, lifts it with `..\recomp` (the recompiler, S1), links the result with this runtime and runs it through the module loader, the module registry and a runtime guest thread (`driver.cpp`); results[] must equal the expected output. Requirements besides the runtime's own: `..\recomp`, `..\tools\.venv` (Python with unicorn) and LLVM clang for `aarch64-linux-gnu`; without them CMake skips the directory (`-DNXRT_BUILD_LIFT_TESTS=OFF` skips it on purpose). Expected output: Unicorn for the 15 programs of `recomp/oracle/e2e/progs`, the host's own C++ exceptions for this work's programs. Run one: `build\release\tests\lift\bin\liftrun_20_eh_hier.exe --elf build\release\tests\lift\20_eh_hier.elf --expected build\release\tests\lift\20_eh_hier.expected`. Check a new program on the host first: `tests\tools\native_check.cmd tests\lift\progs\NN_name.cpp`. docs/S3b.md has the details.

Fixtures: `tests\data\fixture_aarch64.elf` is a synthetic AArch64 PIE built from `tests\fixture\fixture.c`; `fixture_nosections.elf` is the same image with the section headers removed. `tests\fixture\make_fixture.cmd` rebuilds both with the LLVM tools and `fixture_info.txt` records their facts. `tests\data\hipc_vectors.h` and `hipc_vectors.md` are generated: run `python -I tests/tools/gen_hipc_vectors.py` from the checkout root, do not edit them by hand. CMake passes `tests\data` to the test binary as `NXRT_TEST_DATA_DIR`.

## Architecture

- Address space: identity mapped, so guest pointers are host pointers. Constants are in `include/nxrt/memory.h` (table below). The heap is one pagefile-backed section with a primary view and alias views of the same pages.
- Module loader (`module.h`): maps each AArch64 ELF64 module at its lifted base, applies relocations and resolves imports in this order: other modules, host data, HLE handlers, weak null, logging stub. Lifted functions are registered in the call table and the init array runs.
- Lifted-module registry (`nxmodule.h`): each module's `NxModule` descriptor is registered with its ABI version (a different version is refused); `host_import(c, NX_IMPORT_ID(module, symbol))` resolves through it. Site hooks (`hooks.h`) attach runtime code to the lifter's `inline`, `midhook` and `entryhook` sites by guest address.
- C++ exceptions (`eh.h`): the runtime is the unwinder (`_Unwind_RaiseException` and `_Unwind_Resume` over the lifter's unwind tables, Itanium typeinfo matching, `nx_catch_call` setjmp frames); libc++abi stays guest code. docs/S3b.md describes the boundary.
- Dispatch (`dispatch.h`): `call_indirect` looks up the guest address in a hash table of lifted functions. Host functions get placeholder addresses (HLE thunks, 16 bytes each, inside the HLE region).
- SVC table (`svc.h`): handlers indexed by SVC number, registered in four groups (memory, info, thread, IPC). Unknown numbers are logged as `UNKNOWN SVC` with their arguments; the number of log lines per number is limited by `[svc] unknown_log_limit`.
- Kernel (`kernel.h`, `sched.cpp`, `thread.cpp`): handle table, one Win32 thread per guest thread. The console core model maps the three application cores onto host processors and the HOS priorities onto six Windows priority bands. The system tick is 19.2 MHz.
- Sync (`sync.h`): nn::os-style mutexes, condition variables, events and semaphores are keyed by guest address in a host-side table. The guest layout is never read or written.
- IPC (`ipc.h`, `hipc.h`): HIPC messages are parsed from the thread's command buffer and routed by session to a service or a domain object. `sm:` is the service manager. Every other requested service is a stub that logs each command and answers with `kStubService` (0x0BCBD5).
- Diagnostics (`diag.h`, `symbols.h`): a crash handler writes the `HOST EXCEPTION` report. A freeze watchdog dumps host and guest thread state when `diag_frame_tick()` stops advancing. A memory guard compares the private commit with a limit. A ring keeps recent file accesses. Host frames are named from the linker map next to the exe.

Address space (`include/nxrt/memory.h`):

| Region | Base | Size |
|---|---|---|
| Main module image (`kMainBase`) | 0x7100000000 | per module, at its lifted address |
| HLE thunks (`kHleBase`) | 0x7400000000 | 0x1000000 (16 MiB) |
| SDK modules (`kSdkBase`) | 0x7500000000 | per module |
| Heap view (`kHeapBase`) | 0x7800000000 | 0x200000000 (8 GiB maximum) |
| Alias views (`kAliasBase`) | 0x7A00000000 | 0x200000000 (8 GiB) |
| Guest thread stacks (`kStackBase`) | 0x7C00000000 | 0x100000000 (4 GiB) |
| Kernel region (`kKernelBase`) | 0x7E00000000 | 0x100000000 (4 GiB) |

## Configuration

Settings come from an ini file (`nxrt.ini` in the working directory, or the path in `NXRT_INI`) and from environment variables. An environment variable wins over the ini value, and the ini value wins over the default. Every key and variable is listed in docs/CONFIG.md.

## Provenance

Every source or header file that names an upstream project in its comments is listed below. The licence column gives the licence of the named upstream project. "Pattern", "idea", "structure" and "adapted" are the words the file headers use.

| File | Source (repository, file, use) | Source licence |
|---|---|---|
| `include/nxrt/diag.h` | smo-recomp `runtime/core/hostrt.c` (crash_filter), `docs/debugging.md` (ideas); SuperMarioWonderRecomp `runtime/src/kernel_watch.cpp`, `runtime/src/hle_fs.cpp` (ideas); ssbu-recomp `tests/DEBUG_MODE.md` (idea) | GPL-3.0; MIT; GPL-3.0 |
| `include/nxrt/dispatch.h` | smo-recomp `runtime/core/hostrt.c` (pattern); SuperMarioWonderRecomp `runtime/src/loader.cpp` (pattern) | GPL-3.0; MIT |
| `include/nxrt/ipc.h` | SuperMarioWonderRecomp `runtime/src/ipc.h`, `runtime/src/ipc.cpp` (structure); ScarletNX service registry (idea, no file named) | MIT; GPL-3.0-or-later |
| `include/nxrt/kernel.h` | SuperMarioWonderRecomp `runtime/src/kernel.cpp`, `runtime/src/kernel_state.h` (pattern); smo-recomp `runtime/os/os_threads.c` (pattern) | MIT; GPL-3.0 |
| `include/nxrt/memory.h` | smo-recomp `runtime/core/hostrt.c`, `runtime/os/smo_nn.c` (pattern); SuperMarioWonderRecomp `runtime/src/memory.cpp` (pattern) | GPL-3.0; MIT |
| `include/nxrt/module.h` | smo-recomp `runtime/core/hostrt.c` runtime_init (pattern); SuperMarioWonderRecomp `runtime/src/loader.cpp` (pattern) | GPL-3.0; MIT |
| `include/nxrt/svc.h` | SuperMarioWonderRecomp `runtime/src/kernel.cpp` svc_impl (pattern); smo-recomp unknown-import reporting (idea, no file named) | MIT; GPL-3.0 |
| `src/core/abi_glue.cpp` | smo-recomp `runtime/core/hostrt.c` (pattern) | GPL-3.0 |
| `src/core/hooks.cpp`, `include/nxrt/hooks.h` | ssbu-recomp `tools/lifter/relift_sites.py` (the site-list scheme the lifter follows; no code used) | GPL-3.0 |
| `src/core/nxmodule.cpp`, `include/nxrt/nxmodule.h` | smo-recomp `runtime/core/hostrt.c` (module tables registered at start-up, imports bound by name: pattern) | GPL-3.0 |
| `src/eh/catch_call.cpp`, `src/eh/unwind.cpp`, `include/nxrt/eh.h` | recomp `oracle/e2e/rt/eh_rt.cpp` (S1, table walk and landing protocol: reworked and extended); ssbu-recomp `nro_catch_call` (name and idea only); Itanium C++ ABI and LLVM libc++abi `cxa_exception.h`, `cxa_personality.cpp` (header layout and two-phase behaviour: read, not copied); ssbu-recomp `runtime/eh_runtime.h` (facts only: the SDK's older `__cxa_exception` layout, no handler means terminate without unwinding) | GPL-3.0-or-later; GPL-3.0; Apache-2.0 with LLVM exception |
| `src/eh/typeinfo.cpp` | LLVM libc++abi `private_typeinfo.cpp` (can_catch, has_unambiguous_public_base, process_found_base_class: behaviour followed, code reimplemented over guest memory); ssbu-recomp `runtime/nncxxrt.c` (facts only: the non-unique-name bit 63 of SDK typeinfos) | Apache-2.0 with LLVM exception; GPL-3.0 |
| `tests/lift/guest/rt2.h` | LLVM libc++abi `cxa_exception.cpp` (handlerCount protocol of __cxa_begin_catch / end_catch / rethrow: behaviour followed, reimplemented freestanding) | Apache-2.0 with LLVM exception |
| `tests/tools/guest_build.py`, `tests/tools/unicorn_expect.py` | import recomp `oracle/e2e/build_prog.py` and `unicorn_side.py` at build time (S1; not copied) | GPL-3.0-or-later |
| `src/core/dispatch.cpp` | smo-recomp `runtime/core/hostrt.c` (pattern); SuperMarioWonderRecomp `runtime/src/loader.cpp` (pattern) | GPL-3.0; MIT |
| `src/diag/crash.cpp` | smo-recomp `runtime/core/hostrt.c` crash_filter (idea; written from scratch) | GPL-3.0 |
| `src/diag/memguard.cpp` | SuperMarioWonderRecomp `runtime/src/memguard_win.cpp` (adapted) | MIT |
| `src/diag/stackwalk.cpp` | smo-recomp `runtime/core/hostrt.c` crash_filter (pattern) | GPL-3.0 |
| `src/diag/watchdog.cpp` | SuperMarioWonderRecomp `runtime/src/kernel_watch.cpp` (idea adapted) | MIT |
| `src/ipc/ipc.cpp` | SuperMarioWonderRecomp `runtime/src/ipc.cpp` (structure adapted) | MIT |
| `src/ipc/sm.cpp` | SuperMarioWonderRecomp `runtime/src/ipc.cpp` (structure adapted) | MIT |
| `src/kernel/handles.cpp` | SuperMarioWonderRecomp `runtime/src/kernel.cpp` handle_add, handle_get, handle_close (pattern) | MIT |
| `src/kernel/kernel_sync.cpp` | SuperMarioWonderRecomp `runtime/src/kernel.cpp` (logic ported, reworked) | MIT |
| `src/kernel/sched.cpp` | smo-recomp `runtime/os/os_threads.c` (measurement cited, no code named) | GPL-3.0 |
| `src/kernel/svc.cpp` | SuperMarioWonderRecomp `runtime/src/kernel.cpp` svc_impl (dispatch pattern) | MIT |
| `src/kernel/svc_info.cpp` | SuperMarioWonderRecomp `runtime/src/svc_process.cpp` (semantics adapted) | MIT |
| `src/kernel/svc_memory.cpp` | SuperMarioWonderRecomp `runtime/src/kernel.cpp`, `runtime/src/svc_process.cpp` (semantics adapted) | MIT |
| `src/kernel/svc_thread.cpp` | SuperMarioWonderRecomp `runtime/src/kernel.cpp` svc_impl (structure adapted) | MIT |
| `src/kernel/thread.cpp` | SuperMarioWonderRecomp `runtime/src/kernel.cpp` new_thread, thread_main (pattern); smo-recomp `runtime/os/os_threads.c` (pattern) | MIT; GPL-3.0 |
| `src/kernel/time.cpp` | smo-recomp `runtime/os/os_threads.c` sleep_ns (pattern); SuperMarioWonderRecomp `runtime/src/kernel.cpp` system_tick (pattern) | GPL-3.0; MIT |
| `src/loader/module.cpp` | smo-recomp `runtime/core/hostrt.c` runtime_init (pattern); SuperMarioWonderRecomp `runtime/src/loader.cpp` (pattern) | GPL-3.0; MIT |
| `src/mem/memory.cpp` | smo-recomp SetMemoryHeapSize behaviour (body comment at line 456, no file named) | GPL-3.0 |
| `include/nxrt/frame_limiter.h` | Blur Busters G-SYNC 101 guidance (the VRR cap formula refresh - refresh^2 / 3600; a published rule, no code) | n/a |
| `src/gfx/vk_device.cpp` | SuperMarioWonderRecomp `docs/nvn/DEBUG-2.md` entries 69 (NVIDIA desktop crash on an ASTC image) and 70 (integrated GPU listed first), ideas only, code written from scratch | MIT |
| `src/gfx/nvn/nvn_loader.cpp` | smo-recomp `runtime/gfx/nvn_stub.c` (idea: log the first calls of an unimplemented entry point with the caller, count the rest); the argument decoding by signature class is new | GPL-3.0 |
| `src/gfx/nvn/nvn_table.inc`, `src/gfx/nvn/nvn_sizes.inc`, `scripts/gen_nvn_table.py` | Generated. Function names, argument classes and object sizes read from mp1r-practice-mod `src_framework/nvn/nvn.h` and `nvn_FuncPtrBase.h` (nvn.h carries an MIT notice, Copyright (c) 2020 h1k421; the repository itself is GPL-2.0); extra names from smo-recomp and ssbu-recomp `runtime/` (GPL-3.0), SuperMarioWonderRecomp `runtime/src/nvn` (MIT) and LASRecomp `runtime/nvn` (MIT). Names, types and sizes are facts of the NVN interface; no source text is copied | MIT; GPL-2.0; GPL-3.0; MIT |
| `src/gfx/scene.cpp` | tools/tiling (`tegra_tiling.h`, compiled in as source from the tools repository) for the block-linear round trip of the test texture | GPL-3.0-or-later |

Third-party code fetched at configure time (graphics shell). Each archive is downloaded by URL and checked against a SHA256 in `cmake/gfx_deps.cmake`; notices are in THIRD_PARTY_NOTICES.md.

| Component | Pin | URL / SHA256 | Licence |
|---|---|---|---|
| SDL3 | 3.4.18 (released 2026-10-02), built from source, static | github.com/libsdl-org/SDL release-3.4.18 `SDL3-3.4.18.tar.gz`, 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 | zlib |
| Vulkan-Headers | vulkan-sdk-1.4.363.0 | github.com/KhronosGroup/Vulkan-Headers tag archive, f4be95220ff0ee0b1c620301ff4fe9ae48592d71f4c30f2d5c571351a153ae81 | Apache-2.0 OR MIT |
| volk | vulkan-sdk-1.4.363.0 | github.com/zeux/volk tag archive, 889b90fdcbe9cee7b118339d640c6895b7be5c920d309c44b744692063275b3d | MIT |
| VulkanMemoryAllocator | v3.4.0 | github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator tag archive, ce9a61ea5e72284ac9ccc152c214eef381ab57bfd7383f20232e5a652e3afda6 | MIT |
| Dear ImGui | v1.92.9b | github.com/ocornut/imgui tag archive, e1c46d676c2bcb7ced847ba27f50553e33a19db97b3cadaec7f8be64449139f8 | MIT |

The Vulkan SDK (validation layer, `glslc` for `scripts/gen_scene_shaders.py`) is a development tool and is not shipped.

External specifications used as references:

- Itanium C++ ABI, "Exception Handling" (https://itanium-cxx-abi.github.io/cxx-abi/abi-eh.html) and its RTTI chapter: personality contract, landing pad registers (x0 exception, x1 selector), typeinfo class layouts. Cited in `include/nxrt/eh.h` and `src/eh/`.

- Switchbrew IPC_Marshalling page (https://switchbrew.org/wiki/IPC_Marshalling): HIPC layouts, cited in `include/nxrt/hipc.h`. No code copied.
- Switchbrew SVC page (https://switchbrew.org/wiki/SVC): SVC numbers, names and calling convention, cited in `src/kernel/svc_names.inc` and `include/nxrt/svc.h`. No code copied.
- Atmosphere libvapours `svc_results.hpp`: numeric result values, cited in `include/nxrt/kernel.h`. Values only.
- libnx `sf/hipc.h` and `sf/cmif.h`: layouts. The packing functions in `tests/tools/gen_hipc_vectors.py` are re-implemented from these headers, as that script's header states, so this is not "no code copied". libnx is ISC licensed; its notice is in THIRD_PARTY_NOTICES.md.

## Status and gaps

- No real service behaviour: services are stubs, by design.
- Audio is out of scope of this library. Graphics, platform input and the NVN front end are the graphics shell (`nxrt_gfx`, docs/S4b.md): the NVN entry points are logging stubs until a back end fills them in; game input mapping is not part of it.
- Kernel objects for guest-side servers (CreatePort, ReplyAndReceive) are not implemented and are logged as unknown SVCs.
- C++ exceptions: dynamic exception specifications, `_Unwind_ForcedUnwind` and `_Unwind_Backtrace` are not supported; the `__cxa_exception` layout, the typeinfo vtable symbols and the place of libunwind in the game are assumptions until the dump exists (docs/S3b.md, Gaps).
- A lifted module's ABI version is passed by the caller because `NxModule` carries none (lifter note in docs/S3b.md).
