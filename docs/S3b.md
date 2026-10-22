# S3b notes: runtime and recompiler integration (ABI version 2, C++ exceptions)

Part S3b of the Metroid Prime Remastered recompilation. Scope: port the runtime to the recompiler's ABI version 2, implement C++ exception support, and run the recompiler's end-to-end programs through the real runtime. Still no game binary: every check ran on test programs.

## Result (2026-10-21)

- Clean Release build plus `ctest`: 46 entries (17 original groups, 4 new unit groups, 25 lifted-program entries). `nxrt_tests --all`: 314 cases, 0 failed (255 before this work).
- Rerun everything: `pwsh scripts\build.ps1 -Clean -Test` (about 95 s clean, most of it lifting and compiling the 23 test programs). Lifted programs only: `ctest --test-dir build\release -R lift_` (under a second).
- The kernel_sync and kernel_threads groups are timing tests of the earlier stream; on a busy machine (other builds running) `kernel_sync.mutex_mutual_exclusion` came close to its 20 s watchdog (14.6 s idle, over 20 s once) and `kernel_threads.system_tick_monotonic_and_rate` failed once on a tolerance. Both passed when rerun alone. Not touched here.

## What changed

| Area | Files | Behaviour |
|---|---|---|
| ABI 2 | `include/nxrt/abi.h` | asserts `NXRECOMP_ABI_VERSION == 2`; `nxrt::kAbiVersion` |
| Module registry | `include/nxrt/nxmodule.h`, `src/core/nxmodule.cpp` | `nxmodule_register(image, &nxmod_x, abi_version)` validates, refuses and registers; lookup by index, name or pc |
| Imports | `src/core/abi_glue.cpp` | `host_import(c, import_id)` resolves `NX_IMPORT_ID(module index, symbol)` through the registry; the "guess the module" code is gone |
| Site hooks | `include/nxrt/hooks.h`, `src/core/hooks.cpp` | `nx_site_inline`, `nx_site_midhook`, `nx_hook_entry`; `hook_add_inline/mid/entry(addr, fn, user)` for the PC-feature code |
| Exceptions | `include/nxrt/eh.h`, `src/eh/*.cpp` | unwinder, typeinfo matching, `nx_catch_call`, `_Unwind_*` handlers |
| Loader | `src/loader/module.cpp` | a static ET_EXEC image with no dynamic section now loads `.symtab` (names in backtraces, `module_find_symbol` for tests); `module_symbol_overridden()` |
| Tests | `tests/test_{hooks,nxmodule,eh_typeinfo,eh}.cpp`, `tests/lift/`, `tests/tools/` | see below |

### Import resolution

`NX_IMPORT_ID(m, s)` names symbol `s` of the module registered with `NxModule.index == m`. Order: a symbol forced to the host with `module_override_symbol`, then a function that another registered module defines under that name (`DynSym.defined`, `value` rebased when it is below the module base), then an ELF symbol of another mapped image that is a registered function, then the HLE handler or logging stub of that name. The result is cached per (module, symbol) and the caches are cleared by every registration and every override (debug note 006). Indexes must be below 4096 (12 bits of the id) and unique.

### ABI version check

The `NxModule` struct carries no version, so the caller passes the `NXRECOMP_ABI_VERSION` its lifted code was compiled with (`NXRT_REGISTER_NXMODULE(image, nxmod_main)` does this when the game exe compiles the generated sources with the same `ctx.h`). A different value is refused with `RegStatus::AbiMismatch`, nothing is registered, and the log names the module. Integration test `lift_00_smoke_abi_mismatch` tries versions 1, 3 and 0 and then runs the module normally. A stale prebuilt library would still pass: the lifter should emit its version next to `nxmod_<name>` (see Lifter notes).

### Site hooks

Lock-free reads of an immutable snapshot (address to chains per kind). Entry and midhook chains run in registration order and stop at the first hook that returns nonzero; inline hooks all run. A hook may register or remove hooks. Counters per hook and per site kind are in the end-of-run summary.

## C++ exceptions: the boundary between guest and runtime

The guest (the recompiled game) keeps libc++abi: `__cxa_allocate_exception`, `__cxa_throw`, `__cxa_begin_catch`, `__cxa_end_catch`, `__cxa_rethrow`, `std::terminate`, the `__cxa_exception` header and the per-thread caught-exception stack. The runtime is the unwinder underneath: `_Unwind_RaiseException`, `_Unwind_Resume`, `_Unwind_Resume_or_Rethrow`, `_Unwind_DeleteException`. The guest's own libunwind and `__gxx_personality_v0` never run: the lifter cannot lift the register restore plus jump into a landing pad, so the runtime does the walk with the lifter's tables (`NxModule.eh`) and re-enters the lifted code itself.

How the guest reaches the runtime (all three end in the same functions, `eh::unwind_raise_exception` etc.):

1. an import by name: `register_unwind_hle()` registers HLE thunks under the libgcc names and forces them with `module_override_symbol`;
2. an entry hook on the guest's function: `hook_add_entry(addr, eh::hook_unwind_raise_exception)` (the tests do this on the stubs of `tests/lift/guest/rt2.h`, with `entryhook` lines in a site list);
3. a lifter function override: game config `overrides = {addr: "nxrt_unwind_raise_exception"}` binds the extern "C" function `h_nxrt_unwind_raise_exception(Ctx*)` that the runtime exports (also `..._resume`, `..._resume_or_rethrow`, `..._delete_exception`).

Where the game's libunwind lives (same module as libc++abi or the sdk module) decides which of the three the config uses; it is unknown until the dump exists.

Algorithm (`src/eh/unwind.cpp`): from the registers at the call (pc = x30, sp = x31), find the FDE of pc-1 across the registered modules, read the CFI row (CFA = x[cfa_reg] + off, callee-saved registers at CFA + off, caller pc = restored x30), look at the LSDA call site covering pc-1 and its action chain.
- Phase 1 (search): walk until a catch clause matches (`eh::can_catch` over the thrown type, `catch(...)` = type 0, cleanups ignored). No match: log "uncaught C++ exception of type ..." with a guest backtrace and return `_URC_END_OF_STACK` (5) with the guest untouched, so the guest's `__cxa_throw` calls `std::terminate` with the throw site still on the stack. When a handler is found, `handlerSwitchValue`, `catchTemp` and `adjustedPtr` (offsets in `eh::AbiLayout`) are written into the exception header, which is what libc++abi's personality leaves for `__cxa_begin_catch`.
- Phase 2 (cleanup): walk again; at every frame with a cleanup pad land with selector 0, at the handler frame land with the filter value. Landing sets x0 = the `_Unwind_Exception*`, x1 = selector, restores x19..x30, the low halves of d8..d15 and SP of that frame.
- Landing target: a function entered through `nx_catch_call` has a setjmp frame keyed by its entry SP (= its CFA); the unwinder stores the pad and longjmps there and `nx_catch_call` calls `body(c, pad)`. Otherwise the pad is found in `NxModule.resume` and `u_<entry>(c, pad)` is called. A cleanup pad ends in `_Unwind_Resume`, which repeats phase 1 from the cleaned function's caller (deterministic, so no state is kept between the phases) and continues.
- State is per thread (`thread_local` catch-frame stack, legacy type table); one host thread is one guest thread.

Itanium typeinfo matching (`src/eh/typeinfo.cpp`, `can_catch`): exact type (same object or same name; a name starting with `*` is internal linkage and only equals itself); derived-to-base for single, multiple and virtual inheritance with the "unambiguous public base" rule (same subobject reached twice counts once; two different subobjects is ambiguous; a private/protected path does not match); pointers: same type, qualification conversion (qualifiers may be added, not removed), derived-to-base pointer conversion with adjustment, `void*`, pointer to pointer with the const rule at every level, `nullptr_t`; pointers to members; `catch(...)`; foreign exception classes match only `catch(...)` and cleanups; dependent exceptions (`std::exception_ptr`) take type and object from the primary. The kind of a typeinfo is told by its vptr: `discover_typeinfo_vtables()` finds the nine `_ZTVN10__cxxabiv...` vtable symbols with `module_find_symbol` / the module exports; without them a typeinfo only matches itself (log line at start-up).

The uncaught report also asks `std::exception`-derived types for `what()` (a guest call from the failure path; `[eh] call_what = 0` turns it off).

Rethrow and nested exceptions need nothing special in the runtime: `__cxa_rethrow` raises the same `_Unwind_Exception` again (a new phase 1), and nested exceptions are separate raises on the same thread; both are exercised by `22_eh_rethrow_nested`.

The S1 programs 12 to 14 use a simpler guest side (`rt.h`: two host services at fixed addresses, `raise(object, typeinfo)` and `resume(object)`). `eh::legacy_raise/legacy_resume` serve them through the same core; the driver registers them at `0x7110000000` and `+16` with `fn_register`. An uncaught exception there is std::terminate in the runtime (`terminate_process`: log, backtrace, exit code 134).

Layout (`eh::AbiLayout`): `__cxa_exception` is 0x80 bytes, `_Unwind_Exception` at +0x60, the thrown object at +0x80 (LLVM libc++abi `cxa_exception.h`, LP64, read 2026-10-21). Two generations differ in the fields in front of the unwind header: newer libc++abi starts the struct with a `reserve` word (`exceptionType` at -0x50 from the `_Unwind_Exception`, `adjustedPtr` at -0x8), the older one does not (-0x58 and -0x10); the SDK that ssbu-recomp saw is the older one (`refs/ssbu-recomp/runtime/eh_runtime.h`). The runtime does not guess: at the first exception it checks which candidate points to a readable typeinfo (a known kind when the vtables are configured) and keeps that layout (log line "exception header layout: ..."); `set_abi_layout` fixes it by hand. Exception classes `CLNGC++\0` and `GNUCC++\0`, low byte 1 = dependent. Typeinfo name pointers may carry bit 63 (ARM64 libc++ marks non-unique names with it; the SDK does); the matcher masks it. The dump has to confirm all of it.

## Tests

| CTest entries | What |
|---|---|
| `hooks` (12 cases) | registration, ordering, chain stop, removal, counters, 4 threads against concurrent registration |
| `nxmodule` (16) | ABI mismatch, invalid and duplicate descriptors, (module, symbol) import resolution, cross-module exports, override and cache invalidation |
| `eh_typeinfo` (16) | hand-built typeinfo graphs: single, multiple, virtual diamond, ambiguous, private, pointers, qualification, `void*`, null, nullptr, pointer to pointer, corrupt graphs, names with bit 63 |
| `eh` (15) | the unwinder on a hand-built NxModule: landing with restored registers, selectors, cleanup before handler, no handler, unknown pc, libc++abi header front end (both layouts), dependent and foreign exceptions, nested catch frames, re-landing, terminate backtrace, what() in the uncaught report |
| `lift_<program>` (15) | all of S1's `oracle/e2e/progs` through lifter, real runtime and loader; results[] must equal Unicorn's. 09_tls masks 13 environment-dependent words (TPIDRRO address and the words behind it, hash) |
| `lift_19..26` (8) + 2 | this work's exception programs (`tests/lift/progs`, written against `tests/lift/guest/rt2.h`, a miniature libc++abi with the real header layout): probe, class hierarchies, pointer conversions, rethrow and nested, catch-all and misc, terminate (uncaught, noexcept), 8 guest threads at once; expected results come from the same source compiled for the host with its own C++ exceptions |
| `lift_00_smoke_abi_mismatch`, `lift_24_eh_term_uncaught_exit` | wrong ABI versions refused; exit code 134 and the log of an uncaught throw (child process) |

`liftrun` (tests/lift/driver.cpp) also checks that `test_main` returns with the callee-saved registers and SP it was entered with, which is what a correct unwinder has to restore.

Mutation check of the exception code (planted bug, rebuild, the suite must go red): ignoring the public flag, not restoring callee-saved registers, skipping cleanups, never ambiguous, not dereferencing thrown pointers, ignoring the vbase offset, not restoring SP, wrong selector: all caught (2 to 10 tests each). The ninth, skipping the first frame's call-site table on a raise, is not caught because no test program raises from a function with a landing pad around the raise itself.

## Decisions

- Two-phase search/cleanup, returning `_URC_END_OF_STACK` instead of terminating in the runtime when nothing catches: the guest's libc++abi decides what terminate does (handler, message), and the throw site stays on the stack for the backtrace.
- Hook semantics for several hooks on one address (ordered chain, first nonzero wins for entry and mid, all for inline).
- Exception pointer in x0 is whatever the guest passed (the `_Unwind_Exception*`), the header fields are written only for native libc++abi exception classes.
- Frame 0 of a raise is searched like any other frame; a resume skips the LSDA of the cleaned function (its `_Unwind_Resume` call is never covered by a pad).
- Names starting with `*` compare by object, all other typeinfo names by string (the console's duplicated weak typeinfos across modules would otherwise fail).
- The tests build the guest programs with clang, lift them as part of the normal build and keep every generated file in `build\`; the expected results are produced during the build (Unicorn or the native run), not committed.
- The static test ELFs get `.symtab` symbols from the loader so that backtraces and `module_find_symbol` work on them.
- `tests/*.cpp` is now a non-recursive glob (debug note 008).

## Lifter notes (recomp is read-only here; for later)

1. Emit the ABI version next to `nxmod_<name>` (for example `extern "C" const uint32_t nxmod_<name>_abi = NXRECOMP_ABI_VERSION;`) so a stale prebuilt lifted library is refused. The registry takes the number as an argument already.
2. `DynSym.value` of a defined symbol is absolute for static ELFs and module-relative for NSO-derived ELFs in the S1 samples; the runtime rebases values below the module base. Please state which it is for `nx_elf` / `nso` inputs.
3. `NxEh.types` for a PIE image: the typeinfo addresses must be final addresses after R_AARCH64_RELATIVE relocations (the runtime compares them with pointers read from the live typeinfo objects). The static test ELFs prove the static case only.
4. Exception specifications (`filter < 0`) have no type list in `NxEh`; the runtime treats them as never matching and logs once.
5. `h_<name>` overrides work for `_Unwind_*` as the config seam; no lifter change needed.

## Gaps

| Gap | Why |
|---|---|
| Dynamic exception specifications (`throw(X)`, filter < 0) and `__cxa_call_unexpected` | no type list in the tables; rare in modern code |
| `_Unwind_ForcedUnwind` (pthread cancellation), `_Unwind_Backtrace`, `_Unwind_GetIP` and friends | not implemented; imports of them hit the logging stub |
| Exceptions propagating through HLE host frames | the longjmp skips host destructors and locks held by an HLE function between two guest frames; avoid in HLE code that calls back into the guest |
| `AbiLayout` (both known generations are handled), the typeinfo vtable symbol names, where libunwind lives | unconfirmed until the dump; all three are data, not code |
| Dependent exceptions, foreign classes and the hook-based `h_nxrt_*` overrides are tested by unit tests with hand-built headers, not by a lifted program | rt2.h has no `std::exception_ptr` |
| `nxmodule_at` is a linear scan (at most 64 modules) per frame | fine for the handful of NSOs |
