# S4b notes: graphics runtime shell

Part S4b of the Metroid Prime Remastered recompilation. Scope: the game-independent front end that the NVN back end will plug into: SDL3 window and input, a Vulkan 1.3 device layer, the NVN entry-point shell, a Dear ImGui overlay, and capture hooks for tests. No game binary existed during the stream. Everything was checked with a synthetic test scene.

## Result (2026-10-21)

- Library `nxrt_gfx`, test executable `nxrt_gfx_tests` (8 CTest groups, 58 cases) and the demo `nxrt_gfx_demo`. The 17 OS-layer groups still pass.
- NVN shell: 523 entry points, every one answered by `nvnBootstrapLoader` and `nvnDeviceGetProcAddress` with an HLE thunk that logs the call until it is implemented. 37 object sizes.
- Rerun everything: configure once (see "Building a worktree" below), then `cmake --build build\release` and `ctest --test-dir build\release --output-on-failure`. One group: `build\release\nxrt_gfx_tests.exe gfx_scene` (set `NXRT_GFX_HIDDEN=1` to keep the windows hidden). Demo: `build\release\nxrt_gfx_demo.exe --overlay` (F1 shows the menu), or `--headless --frames 300 --screenshot out.png`.

## Frame loop order (low-latency design)

1. `FrameLimiter::begin_frame()` waits for the scheduled start.
2. `platform::pump_events()` reads input into the queue (and the raw mouse accumulator).
3. The caller drains the queue (overlay first), reads `input::mouse_delta_take()` for aim.
4. `Overlay::apply_pending()`, then `Renderer::begin_frame()`, drawing, `end_frame()`.

`scene_runner.cpp` is the reference implementation of this order and measures the time from step 2 to the end of step 4 (`input_to_submit_*`).

## Oracles

| Oracle | Test | Result |
|---|---|---|
| Device picks the discrete GPU by default, honours an override | `gfx_device`: rule on fake lists (iGPU first or last in the list), default device, `integrated` / `Intel` / index / `NXRT_GFX_GPU`, bad override | PASS. RTX 3080 chosen over the UHD 770; every override form selects the right device; a bad override falls back and reports it |
| Zero validation errors over 300 frames of the test scene | `gfx_scene`: offscreen and with a swapchain; `gfx_device.validation_layer_really_reports_errors` proves the layer is live | PASS. 0 errors and 0 warnings in both runs (synchronization validation on); a deliberate bad call is counted |
| Headless screenshot equals the reference image | `gfx_scene`: offscreen, windowed, and the same scene on the integrated GPU | PASS. RTX 3080 offscreen and windowed: mean difference 0.00000, max 0. UHD 770: mean 0.00423, max 1 (limits 0.05 and 4) |
| Present-mode switching and resize do not leak or error | `gfx_present`: 52 swapchains over 400 frames (20 mode switches, 31 resizes), 235-swapchain soak with the overlay, headless resize, minimise and restore, fullscreen toggle | PASS. 0 errors; soak: handles 681 to 682, private bytes +0.33 MiB, 0 live allocations at teardown |
| `nvnBootstrapLoader` returns non-null for every name and the stub logs the call | `gfx_nvn`: all 523 names through both loaders, decoded arguments and caller in the log, quiet after 3 calls, unknown-name policy, `register_impl`, sizes; the table is also compared with the practice mod's header read again at test time | PASS. 523 of 523 answered with a thunk of the right name, 477 of 477 names of the reference header present |
| Frame limiter holds 60/120/240 | `gfx_limiter` (no GPU, thread at high priority under a fully loaded CPU): 60.0000 / 120.0000 / 240.0000 fps, p99 interval deviation 0.2 to 0.5 ms; `gfx_pacing` (whole loop on the renderer, offscreen and a window in immediate mode) | PASS. Loop: 59.99, 120.00, 240.00 fps offscreen and 120.00 windowed in the first attempt, late-start p99 0.1 to 0.4 ms, max 0.56 ms; input is sampled 0.18 ms (max 0.43 ms) before the frame is submitted |
| Existing OS-layer groups | the 17 groups of `nxrt_tests` | PASS. 17 of 17 |

## What exists

| Part | Files | Notes |
|---|---|---|
| Platform and input | `platform.h`, `input.h`, `src/gfx/platform.cpp`, `src/gfx/input.cpp` | SDL3 3.4.18 static. Window create, resize, minimise, borderless and exclusive fullscreen, relative mouse (`SDL_SetWindowRelativeMouseMode`, hints `SDL_MOUSE_RELATIVE_SYSTEM_SCALE=0` and `SPEED_SCALE=1`, `relative_mouse_is_raw()` checks it), gamepad hot-plug (pads opened on add, closed on remove), Vulkan surface. 4096-event queue with a drop counter, plus a raw mouse delta accumulator for late aim sampling. No game mapping. |
| Frame limiter | `frame_limiter.h`, `src/gfx/frame_limiter.cpp` | Absolute schedule, high-resolution waitable timer for the bulk and a spin for the last stretch (margin adapts to the timer's overshoot), a late frame is made up for by at most 10 % of a period, restart after a 4-period stall. `suggest_vrr_cap()`. Accuracy statistics separate from frame cost. |
| Device layer | `gfx.h`, `src/gfx/vk_device.cpp`, `vma_impl.cpp` | volk + VMA. Instance with validation (and synchronization validation) in debug or by config, GPU rule `pick_gpu` (discrete with most memory, else integrated; override by kind, index or name), eligibility (Vulkan 1.3, dynamic rendering, synchronization2, swapchain, a draw+present queue), `create_image` checks `vkGetPhysicalDeviceImageFormatProperties2` and the enabled compression features first, buffers, one-shot submit, allocator statistics. |
| Renderer | `src/gfx/vk_renderer.cpp` | Swapchain (B8G8R8A8_UNORM) or an offscreen image ring, present modes Fifo / FifoRelaxed / Mailbox / Immediate with fallback, frames in flight (default 2), per-image render-finished semaphores, timestamp queries, recreate on resize / out-of-date / present-mode change, skip while minimised, PNG screenshot and pixel capture. |
| Test scene | `scene.h`, `src/gfx/scene.cpp`, `scene_runner.cpp`, `shaders/`, `scene_shaders.inc` | Clear colour and two quads. The 256x256, 9-level texture is tiled with `tegra::tile`, untiled with `tegra::untile`, compared, and uploaded from the untiled copy (`tools/tiling`, compiled in as source). SPIR-V is generated by `scripts/gen_scene_shaders.py` and committed. |
| Overlay | `overlay.h`, `src/gfx/overlay.cpp` | ImGui 1.92.9b with its Vulkan backend (dynamic rendering). Fed from the input queue, no SDL backend, so it works headless. Toggle key F1 (`[gfx] overlay_key`). Panels: GPU, present mode, frame cap and VRR cap, fullscreen mode, raw mouse, frame times, validation counts, NVN stub counts; `add_panel()` for later streams. Releases relative mouse mode while shown. |
| NVN shell | `nvn.h`, `src/gfx/nvn/nvn_loader.cpp`, generated `nvn_table.inc`, `nvn_sizes.inc`, `scripts/gen_nvn_table.py` | See the next section. |
| Capture and headless | `Renderer::request_screenshot`, `capture_next_frame`, `[gfx] headless`, `[gfx] screenshot` | `NXRT_GFX_HEADLESS=1` runs offscreen without a window; `NXRT_GFX_SCREENSHOT=file.png` captures a frame. |

## NVN entry points

The list is the union of three kinds of source. `scripts/gen_nvn_table.py --refs <refs dir>` regenerates the two `.inc` files (deterministic, asserts that every function in the practice mod's `nvn_FuncPtrBase.h` appears exactly once).

| Source | Names | Signatures |
|---|---|---|
| A: `refs/mp1r-practice-mod/src_framework/nvn/nvn.h` + `nvn_FuncPtrBase.h` (reverse-engineered NVN 53.105 header, used against this game; the Exlaunch-Base and Rando-CodeEdits copies are identical) | 479 (incl. `nvnBootstrapLoader`, `nvnLoadCProcs`) | from the PFN typedefs, reduced to AArch64 register classes |
| B: names registered by `smo-recomp` and `ssbu-recomp` (`runtime/gfx`, `runtime`) | 505, of which 38 are not in A (newer API: queue builder getters, window texture counts, colour reduction, sample shading, GL sync interop, raw storage class) | unknown, the stub dumps x0..x7 |
| C: explicit extras from SuperMarioWonderRecomp and LASRecomp (`nvn*Deferred` binds, `nvnBufferBuilderGetDevice`, `nvnMemoryPoolBuilderGetDevice`) | 6 | unknown |

Total 523; 44 have an unknown signature. Object sizes come from A's `reserved[]` arrays and computed structs (37); eight explicit size statements in other projects all agree (`docs/nvn_struct_sizes.md`). deko3d has no NVN layout facts.

The stub logs `STUB <name>(<decoded args>) returns 0, caller=0x..., call #n` for the first 3 calls of each entry point (`[nvn] log_first_calls`), counts the rest, and returns 0 in x0 and v0. The caller is the BLR address (return address minus 4). A name outside the table is logged as UNKNOWN NVN PROC and answered with a stub (`[nvn] unknown = null` returns null instead). `register_impl(name, fn)` replaces a stub; `implemented_count()` and the overlay show progress.

## Decisions

- Separate library and test executable (`nxrt_gfx`, `nxrt_gfx_tests`) so that the OS layer and its 17 groups do not depend on SDL or Vulkan. Existing `CMakeLists.txt` changes: the recomp include path also searches `../../recomp` (worktrees), the two source globs skip `src/gfx` and `tests/gfx`, and `cmake/gfx.cmake` is included. No other existing file changed except README.md, THIRD_PARTY_NOTICES.md and `docs/CONFIG.md` (rows added).
- Dependencies are fetched by URL + SHA256 into the build directory, archives cached in `%LOCALAPPDATA%\nxrt\deps`. SDL is built from source as a static library. The Vulkan headers come from the pinned Vulkan-Headers, not from the SDK; the SDK is only needed for the validation layer and for regenerating the scene shaders.
- ImGui is fed from nxrt's own input events rather than `imgui_impl_sdl3`, so the overlay works without a window and the SDL backend's own event handling does not compete with the game's. Only the key map and mouse are implemented (no clipboard, IME, gamepad navigation).
- Hidden windows under CTest (`NXRT_GFX_HIDDEN=1`): swapchain creation, resize, minimise and a borderless fullscreen request all work on a hidden window; a hidden window does not change size when made fullscreen, so that follow-up is only checked when the windows are visible (`NXRT_GFX_HIDDEN=0`).
- The reference image is made by the code under test (`NXRT_GFX_UPDATE_REF=1`) after the picture was checked by eye and by pixel asserts. The comparison is tight (mean 0.05, max 4 of 255, 0.05 % of pixels over 8): the UHD 770 differs from the RTX 3080 by a mean of 0.004 and a maximum of 1.
- An unknown NVN name gets a logging stub, not null (a game built for a newer driver may ask for names this list lacks, and a null crashes it where a stub names the culprit).
- `nvnBootstrapLoader` and `nvnDeviceGetProcAddress` count as implemented; every other entry is a stub.

## Building a worktree

`cmake/gfx.cmake` looks for `recomp` and `tools` next to the checkout and, for a worktree under `runtime-wt/<name>`, two levels up. As of 2026-10-21 `recomp/include/nxrecomp/ctx.h` in the working tree is ABI version 2 while `include/nxrt/abi.h` asserts version 1, so nxrt itself does not build against it (not a graphics issue; it needs the OS layer to move to ABI 2). The stream was built and tested against the committed version 1 header (`recomp` commit 8b3b6e2, including `fpexact.h` and `neon.h`) passed as `-DNXRT_RECOMP_INCLUDE=<dir containing nxrecomp/>`:

    cmake -S . -B build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache -DNXRT_RECOMP_INCLUDE=<dir>

(run it after `scripts\env.cmd`). `scripts\build.ps1` does the same minus the cache variable, which stays in the cache of an existing build directory.

## Gaps

| Gap | Why |
|---|---|
| Exclusive fullscreen is implemented but not exercised by a test | it changes the monitor's display mode; the test would flash the user's screen |
| The 60/120/240 fps oracle through the real renderer needs a machine that is not busy | on a shared GPU the frame waits for the GPU, see debug 009; the limiter itself is tested without a GPU (`gfx_limiter`) |
| No Vulkan exclusive-fullscreen extension, no HDR, no `VK_NV_low_latency2` / Anti-Lag | later work (plan phases 7 and 8) |
| 44 NVN entry points have an unknown signature (the stub dumps raw registers) | no public header declares them |
| The overlay has no clipboard, IME or gamepad navigation | not needed for a developer menu yet |
| NVN object sizes cover 37 structs; five placeholders in the header (`NVNtextureSparseTileLayout`, `NVNdrawTextureRegion`, `NVNqueueErrorInfo`, `NVNmappingRequest`, `NVNpackagedTextureLayout`) have none | the source header does not define them |
