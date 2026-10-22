# S6b: boot integration, second pass

Part S6b of the Metroid Prime Remastered recompilation. Branch `boot2` (worktree `runtime-wt/boot2`, contains `nvn`), root repo `game-build/`, `tools/boot`, ``. Debug notes 120 to 123.

## Result (2026-10-22)

The real game boots to the title screen ("PRESS A TO START"), the main menu and, with scripted pad input, into the loading screen, rendered by the NVN-on-Vulkan back end (headless, 1920x1080 image ring, about 136 presented frames per second on the RTX 3080). After the loading screen starts, the Vulkan device is lost (debug note 123, NVN side). Without input the game idles on the title screen for as long as it is left running (98 s checked: 13,445 presents, 1.9 M draws, no crash).

Screenshots (work/run, not committed): title screen, main menu (PLAY / OPTIONS / EXTRAS), loading screen.

## What changed

| Item | Where | Notes |
|---|---|---|
| NVN routing | `game-build/src/mpr_main.cpp` | `module_override_symbol("nvnBootstrapLoader")` plus `fn_register` at the sdk definition. The lifted PLT stub of main calls `host_import`, so the GOT that rtld fills is never read (debug note 120). `nvnDeviceGetProcAddress` is reached through the loader's answers. |
| NVN back end start | same | Device + Renderer + `install_backend`; `[boot] gfx = headless` (default) / `window` / `none`. The window mode creates the SDL window but nothing pumps its events yet. |
| Debug 018 | debug/120.md | No lazy binding involved; root cause in the note. |
| Threads | `src/kernel/svc_info.cpp` | ThreadTickCount (GetThreadTimes, 19.2 MHz ticks; total for -1 or the ideal core, 0 for other cores), current thread count = running threads. |
| NVNtextureView size | `api_texture_builder.cpp` | 16 bytes (debug 121). |
| Copy handles | `services/audio.cpp`, `am.cpp`, `network.cpp` | Services no longer close the client's copy handles (debug 122). |
| SetThreadCoreMask | kernel | ideal core -1 accepted (debug 122). |
| Present flip | `nvn_present*.cpp`, `gen_present_shaders.py` | lower-left origin mode is presented flipped (debug 122); the geometry origin oracle was updated. |
| Timing tests | `tests/test_kernel_threads.cpp`, `tests/gfx/test_frame_limiter.cpp`, `test_gfx_pacing.cpp`, `src/gfx/frame_limiter.cpp` | Mechanisms below. |
| Command line | mpr_main.cpp | `--max-seconds`, `--link`, `--no-nvn` applied after `nxrt::init()` (they were dropped before, so the deadline never fired). |
| Watchdog | `nvn_present.cpp` | `diag_frame_tick()` per presented frame: the freeze watchdog no longer fires on a running game. |

## Tools

| Tool | Use |
|---|---|
| `boot_wt.sh [boot.py args]` | boot.py against the boot2 runtime (build dir `game-build/build/boot2`; env `MPR_RUNTIME`, `MPR_BUILD_DIR` select another runtime checkout for `game-build/scripts/build.ps1` and boot.py). |
| `NXRT_HLE_TRACE=nvn` (`[hle] trace`, `trace_limit`) | every HLE call whose name starts with the prefix is logged with x0..x3, lr and the result. `python nvn_order.py [--seq] [--n N] [log]` prints first-seen order and counts. Works for any prefix (`nn::`, `movie::`). |
| `[boot] press = "A@12,A@22:500"` (`NXRT_BOOT_PRESS`) | scripted controller presses on player 1 (button@seconds[:hold ms]). |
| `[boot] screenshot_at / screenshot_every` | `shot_<n>.png` of the presented frame in the run directory. |
| `[boot] trace_plt = 1` | logs every lazy PLT binding of rtld (there are none). |
| `[boot] dump_killer = 1` | temporary debug 121 helper (dumps CFrameDelayedKiller_NVN slots at a fault); remove when no longer wanted. |
| `plt_sym.py` | names the relocation behind a GOT slot or PLT stub of `work/elf/<mod>.elf`. |
| Deadline report | at `--max-seconds` the log carries the NVN back end counters and the unsupported-operation list. |

## Timing tests (mechanisms)

- `kernel_threads.sleep_ns_and_sleep_svc` and `system_tick_monotonic_and_rate`: a zero or negative sleep is `SwitchToThread()`, and a sleep wake-up can be delayed by any number of scheduler quanta on a busy machine. The tests assert the lower bounds (never early) and that every call returns, with a 2 s / 50x upper bound that only catches a hang or a wrong tick rate.
- `gfx_limiter.two_limiters_two_threads_independent`: the only timing case without `ScopedBoost`; both threads are boosted now. On a rate miss with contention (`overran_fraction > 0.5 %`, the test_gfx_pacing rule) the case skips with the reason.
- `gfx_pacing` accuracy: p99 over about 140 waited frames is one or two samples. The limiter statistics now carry `late_over_half_ms` (fraction of waited frames more than 0.5 ms late); the limit is 2 %.

Checked: kernel_threads 3 unloaded and 12 loaded runs (2x logical cores of busy loops), all green; gfx_pacing and gfx_limiter 3 unloaded and 6 loaded runs, all green.

## Boot stops, classified

| # | Stop | Class | Owner |
|---|---|---|---|
| 1 | GUEST TRAP in the SDK NVN driver | debug 018 / 120; gone with the routing | done |
| 2 | FreeId(0) fault, first frame | NVN struct size (debug 121) | fixed on boot2; audit of the other struct sizes: S4d |
| 3 | svcBreak after OpenAudioRenderer | services closed copy handles (debug 122) | fixed |
| 4 | svcBreak in SfThread::run (SetThreadCoreMask -1) | kernel | fixed |
| 5 | ASTC textures not uploaded ("astcdec-*-shared.dll not found") | mpr.exe lacked the three decoder DLLs | fixed: game-build copies them next to the exe |
| 6 | Vulkan device lost in the loading screen | NVN back end (debug 123) | S4d |
| 7 | About 10 nvdrv ioctls "UNHANDLED, answered with zeros" (types 0x00 nr 1/2/7/9/0xa and 0x48 nr 1: the NVDEC/VIC channel of the multimedia library) | movie playback needs the planned Media Foundation HLE of the `movie::` imports (subsdk0 uses nvhost-nvdec through the sdk's NvRm) | movie stream |
| 8 | 11 NVN procs requested by name that are not in the table (logging stubs, not called so far): nvnProgramGetSubtileSize, nvnTextureBuilderGetViewOffset, nvnCommandBufferOverrideSubtileSize, nvnCommandBufferInsertCheckpointStatic/Dynamic, nvnDeviceSetAutomaticCheckpointsEnable/Mask/SamplingInterval/ByDebugGroup, nvnDeviceSetCheckpointCallStacksEnable/MaxCallStacksDepth | NVN 55 names newer than the table | S4d (add to the table; none was called) |
| 9 | Back end unsupported operations during title, menu and loading: ClearDepthStencil with a partial stencil mask (21,806), storage image / texel buffer descriptors (7,037), occlusion/statistics counters (38), transform feedback buffers (38) | NVN back end | S4d |
| 10 | Black background behind the menu (the front-end scene is a movie or 3D scene) | likely 7 and 9 | S4d |

## Decisions

- Headless back end by default: nothing pumps SDL events on the guest main thread yet; `[boot] gfx = window` creates the window for the integration to wire (the PC-feature stream owns the event loop and the frame limiter).
- `fn_register` plus `module_override_symbol` for the loader instead of an entry hook: the lifter only emits entry-hook sites listed in a site file and none exists for the MPR modules.
- The 16-byte NVNtextureView is packed with 16-bit fields (levels and layers up to 32,767).
- Present flip in the shader, not in the viewport (the shader derives the picture UV from the fragment position).

## Gaps

- No window, vsync or frame limiter in `mpr.exe` (headless ring).
- The input source is the scripted ManualInputSource; real input is the input stream's.
- The path after the loading screen is unseen (device lost).
- game-build depends on the nvn-merged runtime (`nxrt/nvn_backend.h`): build with `MPR_RUNTIME=<boot2>` until the maintainer merges `nvn` into main.
