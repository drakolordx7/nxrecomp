# Configuration reference

Settings come from an ini file and from environment variables. The lookup order for a key is: environment variable, then ini value, then the default in the code. The runtime loads `nxrt.ini` from the working directory unless `NXRT_INI` or `InitOptions.ini_path` names another file; a missing file is not an error. Each key read is logged at Info level under the tag `config` with its value and its source (`env`, `ini` or `default`).

Rules:

- Section and key names are case-insensitive.
- Ini syntax: `[section]` lines, then `key = value`. A `;` or `#` that follows whitespace starts a comment. A quoted value is taken verbatim.
- The environment variable for a key is `NXRT_` plus the upper-case section, an underscore, and the upper-case key, with every non-alphanumeric character replaced by `_`. Example: `[diag] watchdog_seconds` is `NXRT_DIAG_WATCHDOG_SECONDS`.
- Integers accept decimal and `0x` hex. Booleans accept `1`/`0`, `true`/`false`, `yes`/`no`, `on`/`off`. Doubles must parse completely. Text that does not parse keeps the default.

## Keys

| Section | Key | Default | Environment variable | Read by | Meaning |
|---|---|---|---|---|---|
| `log` | `path` | `nxrt.log` | `NXRT_LOG_PATH` | `src/diag/log.cpp` | Log file. An empty value writes no file. |
| `log` | `level` | `info` | `NXRT_LOG_LEVEL` | `src/diag/log.cpp` | Lowest level written: `error`, `warn`, `info`, `debug`, `trace`, or `0` to `4`. An unrecognised value keeps the level set in code. |
| `NXRT_MENUMOUSE_LOG` | `src/gfx/play/menu_mouse.cpp` | Menu pointer debugging: `1` logs what the pointer hits and does, `2` also every widget it looks at, `3` also the parts of each button's box. |
| `NXRT_MENUMOUSE_DUMP` | `src/gfx/play/menu_mouse.cpp` | Logs every widget of each older GUI frame (pause screen, options, map) once: type, name, position, size and where the frame's camera puts it. |
| `memory` | `heap_gb` | `6` | `NXRT_MEMORY_HEAP_GB` | `src/mem/memory.cpp` | Heap section reserve in GiB, from 1 to 1024. Out of range: a warning is logged and 6 is used. Read only when `mem::Config.heap_reserve` is 0. |
| `memory` | `app_memory_mb` | `0` (= 3285, 0xCD500000 bytes) | `NXRT_MEMORY_APP_MEMORY_MB` | `src/kernel/svc_info.cpp`, `svc_memory.cpp` | Physical memory the process is told it has: GetInfo TotalMemorySize, and the ceiling of SetHeapSize together with the module images and 64 MiB. |
| `memory` | `limit_gb` | `8` | `NXRT_MEMORY_LIMIT_GB` | `src/diag/memguard.cpp` | Limit in GB for the process's host memory (private commit without the video memory the process has in use: Windows charges that to the commit too). Zero, negative or non-numeric: 8. |
| `memory` | `poll_ms` | `250` | `NXRT_MEMORY_POLL_MS` | `src/diag/memguard.cpp` | Memory guard poll interval in milliseconds. Zero or negative: 250. |
| `memory` | `fast_query` | `true` | `NXRT_MEMORY_FAST_QUERY` | `src/mem/memory.cpp` | Answer `is_mapped` and `protect` for ranges inside the heap view from a per-page table instead of `VirtualQuery` (which walks every page of the run on a section view: about 75 ms for a 3 GiB heap). `false` goes back to `VirtualQuery`; for before/after measurements. Read once. |
| `latency` | `pace` | `main` | `NXRT_LATENCY_PACE` | `src/gfx/extras/lowlat.cpp`, `src/gfx/play/play.cpp`, `src/gfx/nvn/nvn_present.cpp` | Where the frame limiter waits. `main`: at the game's frame boundary, before the input is sampled. The picture shown was simulated one render time earlier, not one queue depth earlier. `present`: inside the present call, as before. In both modes, a present that no boundary paced still waits in the present call. Read once. |
| `latency` | `reflex_limiter` | `false` | `NXRT_LATENCY_REFLEX_LIMITER` | `src/gfx/extras/lowlat.cpp`, `src/gfx/play/play.cpp` | `true` keeps the frame limiter's wait at the frame boundary while Reflex is on. By default Reflex's own sleep (minimum interval = the frame cap) paces the frame and the limiter waits only when Reflex is not active. Read once. |
| `latency` | `vsync_cap` | `true` | `NXRT_LATENCY_VSYNC_CAP` | `src/gfx/play/play.cpp` | With vsync on and no frame cap below the display's refresh rate, the frame limiter holds 0.05% under the refresh rate so the swapchain queue stays empty (input to present p50 fell from 14 ms to 7 ms at 144 Hz in S21). `false` keeps the old behaviour: no cap, the display alone paces. Read once. |
| `nvn` | `accept_unlinked` | `true` | `NXRT_NVN_ACCEPT_UNLINKED` | `src/gfx/nvn/api_program.cpp` | `nvnProgramSetShaders` returns success for a program whose shader block is missing from the shader cache (the program stays unlinked, its draws are skipped). `false` returns failure, which makes the game rebuild that shader on every bind. Read once. |
| `nvn` | `pool_view` | `true` | `NXRT_NVN_POOL_VIEW` | `src/gfx/nvn/nvn_gpu_pool.cpp` | Import a memory pool for the GPU through a small private view of the heap section instead of the pool's own address in the multi-gigabyte primary view (tens of milliseconds per import against 0.05). `false` for before/after runs. Read once. |
| `nvn` | `tex_prep` | `true` | `NXRT_NVN_TEX_PREP` | `src/gfx/nvn/nvn_texprep.cpp` | Prepare the upload data of block-compressed textures (hash, BC7 cache read, untile) on worker threads as soon as the game initializes the texture, instead of on the render thread at its first use. A result is used only if no CPU write was reported since and the storage still hashes the same. `false` for before/after runs. Read once. |
| `nvn` | `tex_prep_threads` | `0` | `NXRT_NVN_TEX_PREP_THREADS` | `src/gfx/nvn/nvn_texprep.cpp` | Worker threads for `tex_prep`; 0 is a third of the hardware threads, 2 to 8. |
| `nvn` | `tex_prep_mb` | `512` | `NXRT_NVN_TEX_PREP_MB` | `src/gfx/nvn/nvn_texprep.cpp` | Most data, in MB, that finished or queued background results hold at once. Over it, the oldest unused results are dropped and the render thread prepares those textures itself. |
| `nvn` | `tex_prep_min_kb` | `8` | `NXRT_NVN_TEX_PREP_MIN_KB` | `src/gfx/nvn/nvn_texprep.cpp` | Textures with less storage than this are prepared on the render thread (the hand-over costs more than the work). |
| `nvn` | `tex_prep_early_kb` | `1024` | `NXRT_NVN_TEX_PREP_EARLY_KB` | `src/gfx/nvn/nvn_texprep.cpp` | Textures with at least this much storage are prepared as soon as they are registered in a texture pool (and, if sparse, after every batch of tile writes); smaller ones only when a command list binds their handle. |
| `nvn` | `tex_prep_log` | `false` | `NXRT_NVN_TEX_PREP_LOG` | `src/gfx/nvn/nvn_gpu_texture.cpp` | One log line per texture upload: format, size, and whether the data came from a background job or was prepared on the render thread, with the time spent. |
| `nvn` | `tex_prep_verify` | `false` | `NXRT_NVN_TEX_PREP_VERIFY` | `src/gfx/nvn/nvn_texprep.cpp` | Test aid: every background result is compared with a fresh preparation on the render thread; a difference is logged as an error and the fresh data is used. |
| `diag` | `watchdog_seconds` | `10` | `NXRT_DIAG_WATCHDOG_SECONDS` | `src/diag/watchdog.cpp` | Freeze threshold in seconds. Zero or negative disables the watchdog. Read only when the start call passes no value. |
| `diag` | `freeze_resample_s` | `0` | `NXRT_DIAG_FREEZE_RESAMPLE_S` | `src/diag/watchdog.cpp` | When the watchdog fires, wait this many seconds and dump every thread again if there is still no frame tick (a second sample tells a stalled thread from a busy one). 0 = one dump. The sweep sets 4. |
| `diag` | `soak_csv` | empty | `NXRT_DIAG_SOAK_CSV` | `src/diag/gauge.cpp` | Path of a file that receives one JSON line every `soak_interval_s` seconds for long-session monitoring: working set, private bytes, handles, threads, frame times of the window, and every gauge the runtime registers (live Vulkan objects, NVN objects, cache sizes, kernel tables, audio and movie queues). Empty: off, nothing runs. Read the file with `soak_report.py`. |
| `diag` | `soak_interval_s` | `10` | `NXRT_DIAG_SOAK_INTERVAL_S` | `src/diag/gauge.cpp` | Seconds between two lines of the soak file (0.5 or more). |
| `sched` | `console_cores` | `true` | `NXRT_SCHED_CONSOLE_CORES` | `src/kernel/sched.cpp` | `false` turns off the console core model. |
| `sched` | `host_cpus` | empty | `NXRT_SCHED_HOST_CPUS` | `src/kernel/sched.cpp` | Hexadecimal mask of host processors, intersected with the process affinity. Ignored when no bit matches. Empty: the runtime picks processors itself. |
| `sched` | `pick` | `auto` | `NXRT_SCHED_PICK` | `src/kernel/sched.cpp` | Which host processors the three console cores use when `host_cpus` is empty. `auto`: the three fastest free physical cores, one set per running nxrt process. `top`: the highest-numbered processors instead. |
| `sched` | `busy_mutex_hle` | `true` | `NXRT_SCHED_BUSY_MUTEX_HLE` | `src/kernel/busy_mutex.cpp` | `nn::os::LockBusyMutex` runs as host code that gives the processor away while it waits (the SDK's loop is a pure spin and starved the lock holder under the console core model, debug note 240). `false` leaves the SDK code in charge. |
| `sched` | `priority_class` | `auto` | `NXRT_SCHED_PRIORITY_CLASS` | `src/kernel/sched.cpp` | Process priority class: `auto` (high when play mode has a window, otherwise left alone), `high`, `above_normal`, `normal`. The guest threads are pinned with the wake-up boost off, so under background CPU load a woken thread waits a scheduler quantum; see debug note 221. |
| `sched` | `spin_yield` | `true` | `NXRT_SCHED_SPIN_YIELD` | `src/kernel/busy_mutex.cpp` | The guest's `wfe` (and, every 64th time, `yield`) hand the host processor to another ready thread instead of spinning (debug note 420). `false`: a pause only. |
| `sched` | `starvation_guard` | `true` | `NXRT_SCHED_STARVATION_GUARD` | `src/kernel/starvation_guard.cpp` | A guest thread that stays ready without running (because other programs keep the host processor busy) is boosted to top priority for 450 ms, and its yields stop handing the processor over. See debug note 422. |
| `game` | `modcon_query_guard` | `true` | `NXRT_GAME_MODCON_QUERY_GUARD` | `src/gfx/play/play_scenefix.cpp` | A decal render query skips a model scene node that is already in the scene's lists but whose model data has not loaded yet, as the normal draw path does. Without it, such a node crashes the game in `CFrustumPlanes::BoxInFrustumPlanes` (S42). `false` runs the game's code unchanged, for reproducing the crash. Needs the hook sites in `main.sites`. Read once. |
| `sched` | `starvation_ms` | `300` | `NXRT_SCHED_STARVATION_MS` | `src/kernel/starvation_guard.cpp` | How long a guest thread must stay ready and unscheduled before the guard boosts it, counted in 150 ms polls. |
| `ipc` | `stub_result` | `0xBCBD5` | `NXRT_IPC_STUB_RESULT` | `src/ipc/stub.cpp` | Result every stub-service command answers. `0` answers success with an empty reply (bring-up probe: the guest then reads zeros, so it only maps which services it asks for next). |
| `hle` | `trace` | empty | `NXRT_HLE_TRACE` | `src/core/dispatch.cpp` | Name prefix of the imports to log. Every call of an import whose name starts with it is logged in order (`hle-trace #n name(x0..x3) lr -> result`). Empty: off. Use it to see the order in which the game calls an API during start-up. |
| `hle` | `trace_limit` | `20000` | `NXRT_HLE_TRACE_LIMIT` | `src/core/dispatch.cpp` | Number of traced calls logged before `hle` `trace` stops logging. |
| `svc` | `unknown_log_limit` | `100` | `NXRT_SVC_UNKNOWN_LOG_LIMIT` | `src/kernel/svc.cpp` | Each unknown SVC number is logged for its first N calls, then every 1000th call. Read on the first unknown SVC. |
| `process` | `entropy_seed` | `0x1234567890ABCDEF` | `NXRT_PROCESS_ENTROPY_SEED` | `src/kernel/svc_info.cpp` | Seed for GetInfo RandomEntropy, subtypes 0 to 3. |
| `process` | `system_resource_size` | `0` | `NXRT_PROCESS_SYSTEM_RESOURCE_SIZE` | `src/kernel/svc_info.cpp` | Value returned by GetInfo SystemResourceSize. |
| `process` | `program_id` | `0` | `NXRT_PROCESS_PROGRAM_ID` | `src/kernel/svc_info.cpp` | Value returned by GetInfo ProgramId. |
| `process` | `pid` | `0x51` | `NXRT_PROCESS_PID` | `src/kernel/svc_info.cpp` | Process id returned by GetProcessId. The InitialProcessIdRange query uses the built-in `0x51` and ignores this key. |
| `threads` | `host_stack_mb` | `64` | `NXRT_THREADS_HOST_STACK_MB` | `src/kernel/thread.cpp` | Host stack reservation per guest thread, in MiB. Values below 1: 64. |
| `eh` | `call_what` | `true` | `NXRT_EH_CALL_WHAT` | `src/eh/unwind.cpp` | The uncaught-exception report calls `what()` of exception types derived from `std::exception` (a guest call). `false` prints the type only. |
| `gfx` | `gpu` | empty | `NXRT_GFX_GPU` | `src/gfx/vk_device.cpp` | Vulkan device override: `discrete`, `integrated`, an index from the logged GPU list, or part of the device name. Empty: the discrete GPU with the most video memory, else integrated. A value that matches nothing is logged and the default is used. |
| `gfx` | `validation` | `true` in debug builds, `false` otherwise | `NXRT_GFX_VALIDATION` | `src/gfx/vk_device.cpp` | Load VK_LAYER_KHRONOS_validation (with synchronization validation) and count what it reports. Needs the Vulkan SDK; without the layer a warning is logged and the run goes on. |
| `gfx` | `headless` | `false` | `NXRT_GFX_HEADLESS` | `src/gfx/vk_device.cpp`, `src/gfx/scene_runner.cpp` | No window and no surface: rendering goes to offscreen images (screenshots and automated tests). |
| `gfx` | `hidden` | `false` | `NXRT_GFX_HIDDEN` | `src/gfx/platform.cpp` | Create the window hidden. CTest sets it to 1. |
| `gfx` | `present_mode` | `fifo` | `NXRT_GFX_PRESENT_MODE` | `src/gfx/vk_renderer.cpp` | `fifo`, `fifo_relaxed`, `mailbox` or `immediate`. A mode the surface does not offer falls back (mailbox to immediate to fifo). |
| `gfx` | `frames_in_flight` | `2` | `NXRT_GFX_FRAMES_IN_FLIGHT` | `src/gfx/vk_renderer.cpp` | Frames the CPU may record ahead of the GPU, 1 to 3. |
| `gfx` | `overlay_key` | `F1` | `NXRT_GFX_OVERLAY_KEY` | `src/gfx/overlay.cpp` | SDL scancode name of the key that shows and hides the ImGui overlay. |
| `gfx` | `screenshot` | empty | `NXRT_GFX_SCREENSHOT` | `src/gfx/scene_runner.cpp` | PNG path: the frame loop writes the presented image of `screenshot_frame` there. |
| `gfx` | `screenshot_frame` | `60` | `NXRT_GFX_SCREENSHOT_FRAME` | `src/gfx/scene_runner.cpp` | Frame number captured when `screenshot` is set. |
| `gfx` | `log_fps` | `false` | `NXRT_GFX_LOG_FPS` | `src/gfx/vk_renderer.cpp` | `1` writes one line a second with the presented frame rate and the GPU time of the present command buffer (output pass, FSR 1). |
| `nvn` | `log_first_calls` | `3` | `NXRT_NVN_LOG_FIRST_CALLS` | `src/gfx/nvn/nvn_loader.cpp` | Calls of each unimplemented NVN entry point logged at Info; later calls are counted and logged at Trace. |
| `nvn` | `unknown` | `stub` | `NXRT_NVN_UNKNOWN` | `src/gfx/nvn/nvn_loader.cpp` | Answer to an NVN name that is not in the table: `stub` hands out a logging stub (logged as UNKNOWN NVN PROC), `null` returns null. |
| `nvn` | `shader_cache` | `` | `NXRT_NVN_SHADER_CACHE` | `src/gfx/nvn/nvn_shaders.cpp` | Folder with `<code hash>.spv` and `results.tsv` (translated shaders). Empty: `./shader_cache/spirv`, then `shader_cache/spirv` beside the executable. |
| `nvn` | `bc7_cache` | `bc7_cache` | `NXRT_NVN_BC7_CACHE` | `src/gfx/nvn/nvn_texdata.cpp` | Folder of ASTC to BC7 transcodes, keyed by a hash of the source bytes. Empty disables. |
| `nvn` | `bc7_quality` | `3` | `NXRT_NVN_BC7_QUALITY` | `src/gfx/nvn/nvn_texdata.cpp` | bc7e preset 0-6 for transcodes done at run time (the setup tool uses 5). |
| `nvn` | `pipeline_cache` | `pipeline_cache.bin` | `NXRT_NVN_PIPELINE_CACHE` | `src/gfx/nvn/nvn_pipeline.cpp` | VkPipelineCache file, loaded with the first pipeline and written every 2 s while new pipelines appear (the prewarm's background thread does this, so only with `prewarm` on), and at shutdown. Empty disables. |
| `nvn` | `pipeline_keys` | `pipeline_keys.bin` | `NXRT_NVN_PIPELINE_KEYS` | `src/gfx/nvn/nvn_prewarm.cpp` | Key file of the pipeline prewarm: every pipeline created at a draw is appended; the next run builds the recorded pipelines of a program on worker threads when the game sets its shaders. Empty disables the prewarm. Pipelines the key file does not know are linked from shader parts instead (`gpl`) and recorded here for the next run. |
| `nvn` | `prewarm` | `true` | `NXRT_NVN_PREWARM` | `src/gfx/nvn/nvn_prewarm.cpp` | `false` turns the pipeline prewarm (recording and building ahead) off. The pipeline cache file is still used. |
| `nvn` | `prewarm_threads` | `max(1, min(4, cpus / 6))` | `NXRT_NVN_PREWARM_THREADS` | `src/gfx/nvn/nvn_prewarm.cpp` | Worker threads that build recorded pipelines. |
| `nvn` | `gpl` | `true` | `NXRT_NVN_GPL` | `src/gfx/nvn/nvn_gpl.cpp` | Link pipelines from separately compiled parts (VK_EXT_graphics_pipeline_library) when the key file has no pipeline for the draw. The shader parts of a program are compiled on background threads when the game sets its shaders. The draw links them together. Off, or on a GPU/driver without the extension: the whole pipeline is built at the draw as before. |
| `nvn` | `gpl_threads` | `max(1, min(4, cpus / 6))` | `NXRT_NVN_GPL_THREADS` | `src/gfx/nvn/nvn_gpl.cpp` | Background threads that compile the shader parts. |
| `nvn` | `gpl_lto` | `false` | `NXRT_NVN_GPL_LTO` | `src/gfx/nvn/nvn_gpl.cpp` | Ask the driver to optimise the linked pipeline (costs more at the draw, keeps the optimisation data of every part in memory). For comparisons. |
| `nvn` | `gpl_upgrade` | `whole` | `NXRT_NVN_GPL_UPGRADE` | `src/gfx/nvn/nvn_gpl.cpp` | What replaces a pipeline that was linked from parts. `whole`: a background thread builds the whole pipeline from the same key and the next lookup swaps it in (the linked one is destroyed once the GPU is done with it). `lto`: the same thread links the parts again with link-time optimisation (about half the work; the parts keep their optimisation data). `off`: the linked pipeline stays. See `S29.md`. |
| `nvn` | `gpl_upgrade_threads` | `1` | `NXRT_NVN_GPL_UPGRADE_THREADS` | `src/gfx/nvn/nvn_gpl.cpp` | Background threads that build the replacements. |
| `nvn` | `gpl_cold` | `true` | `NXRT_NVN_GPL_COLD` | `src/gfx/nvn/nvn_gpl.cpp` | Compile the shader parts of programs the game has set but not yet drawn with. This is a guess, and it avoids waiting at the first draw. `false`: only programs a command buffer was recorded with are compiled, a few frames ahead. About 10 times less compile work, at the cost of a wait of up to one part at the first draw of a new program. |
| `nvn` | `gpl_ab` | `0` | `NXRT_NVN_GPL_AB` | `src/gfx/nvn/nvn_pipeline.cpp`, `src/gfx/nvn/nvn_gpl.cpp` | Measurement only. N > 0 keeps the linked and the replacement pipeline of every draw and uses one or the other in alternating windows of N presents (log lines "ab: present ..."), so one run compares the two. Report: `gplopt_ab_report.py`. |
| `nvn` | `gpl_ab_pause` | `0` | `NXRT_NVN_GPL_AB_PAUSE` | `src/gfx/nvn/nvn_gpl.cpp` | Measurement only. N > 0: the compile threads rest in every other window of N presents. Slows loading (draws wait for parts); not for play. |
| `nvn` | `compile_priority` | `below` | `NXRT_NVN_COMPILE_PRIORITY` | `src/gfx/nvn/nvn_gpl.cpp` | Thread priority of the threads that build pipelines in the background (the prewarm and the shader-part threads): `below` (below normal), `normal`, `idle` (lowest). |
| `nvn` | `compile_eco` | `true` | `NXRT_NVN_COMPILE_ECO` | `src/gfx/nvn/nvn_gpl.cpp` | Run the threads that build pipelines in the background in Windows efficiency mode (EcoQoS): on a CPU with fast and slow cores they go to the slow ones, so a burst of shader compiles does not take the fast cores from the game. The shader preparation always uses all the speed it can get. |
| `nvn` | `compile_note` | `true` | `NXRT_NVN_COMPILE_NOTE` | `src/gfx/overlay.cpp` | Show a small 'Preparing shaders (N left)' note at the bottom of the screen while the background threads still have shader work queued for more than a second and a half; it stays up for one second after the queue empties. `false` keeps it off. |
| `nvn` | `prepare_cache` | `true` | `NXRT_NVN_PREPARE_CACHE` | `src/gfx/nvn/nvn_gpl.cpp` | The shader preparation (`mpr.exe --prepare-shaders`, the launcher's Prepare shaders step) keeps what it compiled in the pipeline cache file (`pipeline_cache`; about 700 MB for the whole game). `false` keeps nothing: the preparation then only warms the graphics driver's own disk cache, which holds little of it. A cache file this big is read in one go at start and rewritten only after about 3,000 new pipelines. |
| `nvn` | `spirv_salt` | empty | `NXRT_NVN_SPIRV_SALT` | `src/gfx/nvn/nvn_pipeline.cpp`, `src/gfx/nvn/nvn_gpl.cpp` | Measurement only. Adds this text to every shader module as a source-extension string so the driver sees new shaders and compiles them from scratch, as on a player's first run, without touching the driver's cache on disk. Use a new text for every cold run. |
| `nvn` | `pool_prefetch` | `true` | `NXRT_NVN_POOL_PREFETCH` | `src/gfx/nvn/nvn_gpu_pool.cpp` | Import a memory pool into Vulkan on a background thread when a buffer is created over it, instead of on the render thread at the first draw. |
| `nvn` | `shot_dir` | `shots` | `NXRT_NVN_SHOT_DIR` | `src/gfx/nvn/nvn_present.cpp` | Folder that `shot_frames` writes to, relative to the working directory. |
| `nvn` | `shot_frames` | empty | `NXRT_NVN_SHOT_FRAMES` | `src/gfx/nvn/nvn_present.cpp` | Presented frames to save as PNG, counted from 1: a comma list of numbers and `from:to:step` ranges (e.g. `300,1000:2000:50`). Files are `<shot_dir>/<number, 6 digits>.png`. Lets a run be compared frame by frame with a reference capture. |
| `nvn` | `dump_missing` | empty | `NXRT_NVN_DUMP_MISSING` | `src/gfx/nvn/nvn_shaders.cpp` | Folder that receives every shader block the shader cache has no translation for, as `code_<hash>.bin`, `ctrl_<hash>.bin` and `manifest.tsv`, for `spirv_harness --batch`. Empty: off. |
| `nvn` | `trace_frame` | empty | `NXRT_NVN_TRACE_FRAME` | `src/gfx/nvn/nvn_replay.cpp` | Presented frame number, or a comma list (`6050` or `6050,9300`). Every draw, clear, copy and dispatch of a traced frame is logged at Info under `nvn.trace`: programs, render targets, sampled textures, blend and depth state. |
| `nvn` | `trace_after_fs` | empty | `NXRT_NVN_TRACE_AFTER_FS` | `src/gfx/nvn/nvn_replay.cpp` | Hash prefix of a fragment shader. The frame after the first one that draws with that shader is traced, so the trace follows the scene rather than a frame number (which differs between runs). Only fires after `trace_min_sec`. |
| `nvn` | `trace_min_sec` | `0` | `NXRT_NVN_TRACE_MIN_SEC` | `src/gfx/nvn/nvn_replay.cpp` | Seconds after the first draw before `trace_after_fs` can fire. |
| `nvn` | `trace_sec` | `0` | `NXRT_NVN_TRACE_SEC` | `src/gfx/nvn/nvn_replay.cpp` | Traces the first frame presented this many seconds after the game submits its first frame. `0`: off. |
| `nvn` | `trace_dump_tex` | empty | `NXRT_NVN_TRACE_DUMP_TEX` | `src/gfx/nvn/nvn_replay.cpp` | Folder for the textures a traced frame samples: the texture data as prepared for the GPU, and the raw level 0 as the GPU holds it (`gpu_t<id>.bin`), to compare the two. Empty: off. |
| `nvn` | `trace_snap` | empty | `NXRT_NVN_TRACE_SNAP` | `src/gfx/nvn/nvn_replay.cpp` | Draw numbers of the traced frame, a comma list (`253,300`). After each of those draws the colour and depth targets are copied to `snap_d<draw>_t<id>_<format>.png`. |
| `nvn` | `trace_snap_fs` | empty | `NXRT_NVN_TRACE_SNAP_FS` | `src/gfx/nvn/nvn_replay.cpp` | Comma list of fragment-shader hash prefixes. Every draw of the traced frame whose fragment shader hash starts with one of them is snapped, as `trace_snap` does. |
| `nvn` | `trace_snap_sampled` | `0` | `NXRT_NVN_TRACE_SNAP_SAMPLED` | `src/gfx/nvn/nvn_replay.cpp` | `1`: a snapshot draw also copies the textures that draw samples. |
| `nvn` | `trace_tex_events` | `0` | `NXRT_NVN_TRACE_TEX_EVENTS` | `src/gfx/nvn/nvn_texture_registry.cpp` | `1`: logs the life of the 128 x 128 textures with six layers and eight levels at Info under `nvn.tev`: creation, CPU writes, uploads and copies. |
| `prof` | `trace` | empty | `NXRT_PROF_TRACE` | `src/diag/ftrace.cpp` | Frame trace file: zones of the render path, slow SVCs and slow HLE calls with their threads and times, flushed once a second. Analyse with `scripts/prof/ftrace_report.py`. Empty: off (one relaxed load per zone). |
| `prof` | `trace_events` | `6000000` | `NXRT_PROF_TRACE_EVENTS` | `src/diag/ftrace.cpp` | Size of the event array (24 bytes each, allocated when the trace starts); events past it are dropped. |
| `prof` | `stall_ms` | `0` | `NXRT_PROF_STALL_MS` | `src/diag/ftrace.cpp` | With a trace on: when no frame was presented for this many milliseconds, dump every thread's stack to `<trace>.stacks` (up to four times per stall) and, when the stall ends, the CPU time each thread used in it. Analyse with `scripts/prof/stack_report.py`. |
| `prof` | `sample_hz` | `0` | `NXRT_PROF_SAMPLE_HZ` | `src/diag/sampler.cpp` | Sampling profiler for machines without a profiler that can run unelevated: that many times a second every registered thread is stopped briefly and its host stack read; each sample is weighted by the CPU cycles the thread used since its last sample. `0` is off (maximum 2000). The result is written at shutdown to `sample_out`. |
| `prof` | `sample_out` | `prof_samples.folded` | `NXRT_PROF_SAMPLE_OUT` | `src/diag/sampler.cpp` | Folded stacks of the sampling profiler (`label;thread;outermost;...;leaf kilocycles`, one per sample; the label is the scene a test script marked with `@log SCENE start NAME`). `prof_report.py` turns it into tables. |
| `prof` | `gpu_passes` | `false` | `NXRT_PROF_GPU_PASSES` | `src/gfx/nvn/nvn_gpu.cpp`, `src/gfx/nvn/nvn_replay.cpp` | With the frame trace on: GPU timestamps between the render passes of every submission. The trace gets one event `gp:<label>` per interval (`gap`: copies, dispatches and barriers between passes, `pass WxH cN:fmt dfmt`: one render pass, with the draw count as argument, `tail`) so the GPU time can be added up per pass kind. Costs a pipeline drain per timestamp: for profiling only. `scene_report.py --gpu` prints the table. |
| `am` | `resolution` | empty (docked 1920x1080, handheld 1280x720) | `NXRT_AM_RESOLUTION` | `src/services/am.cpp` | Output size as `WIDTHxHEIGHT`, e.g. `1280x720`. Empty or not parseable: the default for the operation mode. |
| `am` | `operation_mode` | `docked` | `NXRT_AM_OPERATION_MODE` | `src/services/common.cpp` | `handheld` sets handheld mode. Any other value is docked. |
| `audio` | `sink` | `auto` | `NXRT_AUDIO_SINK` | `src/services/audio_sink.cpp` | `auto` plays through XAudio2 on the default output device and falls back to the null sink (no sound, a warning in the log) when XAudio2 or a device is missing. `xaudio2` is the same with an explicit name. `paced` is a simulated device: it drains its queue at the sample rate and reports underruns and queue levels, which is how the renderer's timing is checked on a machine without a sound card. `null` discards everything. Tests install their own sink. |
| `audio` | `paced_ppm` | `0` | `NXRT_AUDIO_PACED_PPM` | `src/services/audio_paced.cpp` | Clock error of the paced sink in parts per million (positive: the simulated device runs fast). Used to check that the renderer follows a device clock that is not exactly the host's. |
| `media` | `movie` | `mf` | `NXRT_MEDIA_MOVIE` | `src/media/movie_mf.cpp` | `mf`: the game's `movie::` imports (Extractor, Decoder, MediaData, Buffer) are answered on Windows Media Foundation. `off`: the library compiled into subsdk0 runs (it needs the NVDEC/VIC driver, which does not exist here, so no movie plays). |
| `media` | `movie_log` | `false` | `NXRT_MEDIA_MOVIE_LOG` | `src/media/movie_mf.cpp` | One log line per sample read and per input buffer sent to a decoder. Very verbose. |
| `media` | `movie_preroll` | `true` | `NXRT_MEDIA_MOVIE_PREROLL` | `src/media/movie_mf.cpp` | When the game has selected every audio and video track of a movie, `SelectTrack` waits (at most 2 s) for the first decoded samples, on the thread that sets the movie up, so the main thread's first `GetSampleTime` does not wait for the decoder to start. |
| `media` | `movie_lookahead` | `true` | `NXRT_MEDIA_MOVIE_LOOKAHEAD` | `src/media/movie_mf.cpp` | When every selected track reaches its end, the reader is positioned at the start and the first samples are decoded ahead, so `SeekTo(0)` of a looping movie swaps them in at once. |
| `media` | `movie_async_close` | `true` | `NXRT_MEDIA_MOVIE_ASYNC_CLOSE` | `src/media/movie_mf.cpp` | The extractor destructor returns at once; a short-lived thread joins the reader thread and releases the Media Foundation objects (60 to 80 ms on the calling thread otherwise). |
| `audio` | `wav_dump` | empty | `NXRT_AUDIO_WAV_DUMP` | `src/services/audio_sink.cpp`, `src/services/audio_wav.cpp` | Path of a 16-bit PCM WAV file that receives every frame the game sends to the host sink. Empty: no file. See the audio notes below. |
| `audio` | `volume` | `1.0` | `NXRT_AUDIO_VOLUME` | `src/services/audio_sink.cpp`, `src/services/audio_xaudio2.cpp` | Master volume, from `0.0` (silent) to `1.0`. Values outside that range are clamped. Applied by the XAudio2 sink. |
| `audio` | `device` | `default` | `NXRT_AUDIO_DEVICE` | `src/services/audio_xaudio2.cpp`, `src/services/audio_devices.cpp` | Output device for the XAudio2 sink: `default`, an endpoint id, or part of a device's name. See the audio notes below. |
| `audio` | `channels` | `2` | `NXRT_AUDIO_CHANNELS` | `src/services/audio.cpp`, `src/services/audren/Integration/HostAudioDevice.cpp` | Channel count the game's audio renderer works with: `6` for 5.1, any other value for stereo. |
| `title` | `program_id` | `0x010012101468C000` | `NXRT_TITLE_PROGRAM_ID` | `src/services/common.cpp` | Title id of the game. `[process] program_id`, when set, overrides it. Also the default for `save_owner_id`. |
| `title` | `name` | `Metroid Prime Remastered` | `NXRT_TITLE_NAME` | `src/services/common.cpp` | Title name in the control data. |
| `title` | `display_version` | `1.0.0` | `NXRT_TITLE_DISPLAY_VERSION` | `src/services/common.cpp` | Display version string in the control data. |
| `title` | `supported_languages` | empty (the 13 retail languages) | `NXRT_TITLE_SUPPORTED_LANGUAGES` | `src/services/common.cpp` | Comma list of language names (e.g. `AmericanEnglish,Japanese`), or a hex or decimal mask. Empty, or a list that names no language: the default. |
| `title` | `save_owner_id` | the effective program id | `NXRT_TITLE_SAVE_OWNER_ID` | `src/services/common.cpp` | Owner id of the title's save data. Unset, it follows the program id. |
| `title` | `account_save_size` | `4194304` (4 MiB) | `NXRT_TITLE_ACCOUNT_SAVE_SIZE` | `src/services/common.cpp` | Account save data size, in bytes. |
| `title` | `account_save_journal` | `1048576` (1 MiB) | `NXRT_TITLE_ACCOUNT_SAVE_JOURNAL` | `src/services/common.cpp` | Account save data journal size, in bytes. |
| `title` | `device_save_size` | `0` | `NXRT_TITLE_DEVICE_SAVE_SIZE` | `src/services/common.cpp` | Device save data size, in bytes. `0`: none. |
| `title` | `device_save_journal` | `0` | `NXRT_TITLE_DEVICE_SAVE_JOURNAL` | `src/services/common.cpp` | Device save data journal size, in bytes. `0`: none. |
| `title` | `cache_save_size` | `0` | `NXRT_TITLE_CACHE_SAVE_SIZE` | `src/services/common.cpp` | Cache save data size, in bytes. `0`: none. |
| `title` | `cache_save_journal` | `0` | `NXRT_TITLE_CACHE_SAVE_JOURNAL` | `src/services/common.cpp` | Cache save data journal size, in bytes. `0`: none. |
| `title` | `main_thread_stack` | `1048576` (0x100000) | `NXRT_TITLE_MAIN_THREAD_STACK` | `src/services/common.cpp` | Main thread stack size, in bytes (main.npdm). |
| `title` | `main_thread_priority` | `44` | `NXRT_TITLE_MAIN_THREAD_PRIORITY` | `src/services/common.cpp` | Main thread priority (main.npdm). |
| `title` | `main_thread_core` | `0` | `NXRT_TITLE_MAIN_THREAD_CORE` | `src/services/common.cpp` | Main thread processor number (main.npdm). |
| `system` | `language` | `AmericanEnglish` | `NXRT_SYSTEM_LANGUAGE` | `src/services/common.cpp` | Console language by name, e.g. `Japanese`. A name that matches nothing keeps the default. |
| `system` | `region` | empty (USA, 1) | `NXRT_SYSTEM_REGION` | `src/services/common.cpp` | Region by name (`Japan`, `USA`, `Europe`, `Australia`, `HongKongTaiwanKorea`, `China`) or number (Japan 0 to China 5). Empty: USA. |
| `system` | `firmware` | `12.1.0` | `NXRT_SYSTEM_FIRMWARE` | `src/services/common.cpp` | Firmware version that `set:sys` GetFirmwareVersion reports. |
| `system` | `nickname` | `nxrt` | `NXRT_SYSTEM_NICKNAME` | `src/services/common.cpp` | Console nickname. |
| `account` | `nickname` | `Samus` | `NXRT_ACCOUNT_NICKNAME` | `src/services/common.cpp` | Nickname of the local user. |
| `account` | `uid` | empty (`00000000000000014d50525265636f6d`) | `NXRT_ACCOUNT_UID` | `src/services/common.cpp` | Local user id as 32 hex digits. Anything that is not 32 hex digits keeps the default. |
| `fs` | `romfs` | `game/romfs` | `NXRT_FS_ROMFS` | `src/services/common.cpp` | Host folder served as the game's RomFS, relative to the working directory. |
| `fs` | `save_dir` | empty (`%APPDATA%\MPRRecomp\save`) | `NXRT_FS_SAVE_DIR` | `src/services/common.cpp` | Folder for save data. Empty: the APPDATA default. |
| `fatal` | `report_path` | unset (`<exe path without extension>_fatal.txt`) | `NXRT_FATAL_REPORT_PATH` | `src/services/logfault.cpp` | Fatal-error report file. Once the key is set, even to empty, its value is used. Empty: no file is written. |
| `fatal` | `terminate` | `true` | `NXRT_FATAL_TERMINATE` | `src/services/logfault.cpp` | `true` ends the process with exit code 5 after a fatal report. `false` returns. |
| `net` | `offline` | `true` | `NXRT_NET_OFFLINE` | `src/services/network.cpp` | `true`: no connection, no friends and no BCAT data (the start-up log line says so). |
| `pl` | `font_dir` | empty | `NXRT_PL_FONT_DIR` | `src/services/pl.cpp` | Folder searched first for `DejaVuSans.ttf` and the shared font dumps. Empty: the search goes on to `fonts\` next to the exe, then `assets\fonts\` under the working directory, then the exe's folder. |
| `play` | `controls` | `controls.ini` | `NXRT_PLAY_CONTROLS` | `src/gfx/play/play.cpp` | Settings file of play mode (controls, sensitivity, display); written with the factory settings when missing. |
| `play` | `capture_mouse` | `true` | `NXRT_PLAY_CAPTURE_MOUSE` | `src/gfx/play/play.cpp` | Capture the pointer (relative mode) while the game is in play. |
| `play` | `pitch_limit_deg` | `0` | `NXRT_PLAY_PITCH_LIMIT_DEG` | `src/gfx/play/play.cpp` | Pitch limit of the mouse aim in degrees; 0 uses the game's tweak value. |
| `play` | `quit_on_close` | `true` | `NXRT_PLAY_QUIT_ON_CLOSE` | `src/gfx/play/play.cpp` | Closing the window ends the process. |
| `play` | `game_base` | `0x7100000000` | `NXRT_PLAY_GAME_BASE` | `src/gfx/play/play.cpp` | Base of the game's main module (hook addresses). |
| `play` | `log_status` | `false` | `NXRT_PLAY_LOG_STATUS` | `src/gfx/play/play.cpp` | One status line per second in the log. |
| `play` | `pointer_draw` | `false` | `NXRT_PLAY_POINTER_DRAW` | `src/gfx/play/play.cpp` | Test aid: draws an arrow at the menu pointer into the game's own frame, so a game screenshot shows where the pointer is (the OS cursor is not part of the frame). |
| `play` | `selftest` | empty | `NXRT_PLAY_SELFTEST` | `src/gfx/play/play_selftest.h` | Window self-test script for the oracles (steps are listed in that header). |
| `play` | `late_sample` | `true` | `NXRT_PLAY_LATE_SAMPLE` | `src/gfx/play/play.cpp` | Pump the window's events again right before the first `ProcessInput` of a frame takes the mouse counts, so the aim includes what the mouse did since the frame boundary. |
| `prompts` | `style` | `auto` | none (controls.ini only) | `src/gfx/play/play_config.cpp` (read from `controls.ini`) | Button icons the game's prompts show. `auto` (the default) shows keyboard and mouse icons until a controller is used, then that controller's icons, and goes back to the keyboard when a key or mouse button is pressed. The other values are `switch` (the game's own icons), `xbox`, `playstation` and `keyboard` (keyboard and mouse). Also accepts `ps`, `ps4`, `ps5`, `dualsense`, `kbm`, `mouse` and `keyboard_mouse`. Stored in `controls.ini` under `[prompts]`, not in `nxrt.ini`. Set it in the launcher (Controls tab) or on the F1 page (Controller tab, Button prompts). Changes apply on the next frame, with no restart. Keyboard icons show the keys the action is bound to: a rebound jump key shows the new key, and a key with no icon of its own gets a plain key icon. Xbox and PlayStation icons follow `[padmap]`. |
| `fps` | `show` | `0` | none (controls.ini only) | `src/gfx/play/play.cpp`, `src/gfx/overlay.cpp` | Frame rate counter in a corner of the game window. Off by default. While it is off, the overlay does one atomic read per frame and nothing else. The hotkey (`key`), the Display tab of the F1 page and the launcher's Extras tab can turn it on or off, and the choice is saved. |
| `fps` | `key` | `F2` | none (controls.ini only) | `src/gfx/play/play.cpp` | Keyboard key that shows and hides the counter. The game never receives it, so do not bind the same key to an action. Any SDL scancode name. |
| `fps` | `corner` | `top_left` | none (controls.ini only) | `src/gfx/overlay.cpp` | `top_left`, `top_right`, `bottom_left` or `bottom_right`. |
| `fps` | `detail` | `1` | none (controls.ini only) | `src/gfx/overlay.cpp` | `0` shows the frame rate only, `1` adds the frame time, and `2` also shows the 1% low (the frame rate of the slowest 1% of frames in the last 5 seconds) and the worst frame time in the last half second. The numbers refresh four times a second. |
| `fps` | `in_screenshots` | `0` | none (controls.ini only) | `src/gfx/overlay.cpp` | `0`: a frame being captured (the test tooling's `@shot`, `[nvn] shot_frames`, or the boot screenshots) is drawn without the counter. `1`: the counter appears in the picture. The self-test step `set:fps_in_shots=1` switches it. |
| `padmap` | `south`, `east`, `west`, `north`, `left_shoulder`, `right_shoulder`, `left_trigger`, `right_trigger`, `start`, `back`, `left_stick`, `right_stick`, `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right` | `default` | none (controls.ini only) | `src/gfx/play/pad_map.cpp` | What each controller button presses on the Switch pad. `default` keeps the built-in layout (`[pad] label_abxy` still decides the face buttons). `none` makes the button do nothing. Or list Switch buttons joined by `+` (`A`, `ZL+L`). Names describe position: `south` is the bottom face button (A on an Xbox pad, cross on a PlayStation pad). Stick directions are not allowed here. A remapped button stops pressing its built-in button. |
| `prompts` | `dir` | empty | `NXRT_PROMPTS_DIR` | `src/gfx/play/prompts.cpp` | Folder searched first for `prompt_icons.pak`, the button icon pack. Empty: the search goes on to `prompts` beside the executable, then `assets/prompts` under the working directory, then the executable's folder. |
| `hdr` | `mode` | `auto` | `NXRT_HDR_MODE` | `src/gfx/extras/extras_state.cpp` | HDR output: `off`, `auto` (HDR when the display and Windows offer it) or `on`. Play mode reads the `[hdr]` section of `controls.ini` instead; this key only applies to code that does not load that file. The environment variable overrides the file in play mode. Applies at the next start. |
| `hdr` | `format` | `auto` | `NXRT_HDR_FORMAT` | `src/gfx/extras/extras_state.cpp` | Swapchain format: `auto` (HDR10 when offered, else scRGB), `hdr10` or `scrgb`. Same reading rules as `hdr` `mode`. Applies at the next start. |
| `hdr` | `peak_nits` | `0` | `NXRT_HDR_PEAK_NITS` | `src/gfx/extras/extras_state.cpp` | Peak brightness in nits: `0` takes the display's reported peak (1000 when it reports none), otherwise 200 to 10000. Same reading rules as `hdr` `mode`. Applies live. |
| `hdr` | `paper_white_nits` | `0` | `NXRT_HDR_PAPER_WHITE_NITS` | `src/gfx/extras/extras_state.cpp` | Brightness of SDR white in nits: `0` takes Windows' SDR content brightness when HDR is on there (203 otherwise), otherwise 80 to 500. Same reading rules as `hdr` `mode`. Applies live. |
| `hdr` | `highlight_boost` | `1` | `NXRT_HDR_HIGHLIGHT_BOOST` | `src/gfx/extras/extras_state.cpp` | How far highlights go above the game's own white: 0 keeps the SDR look, 1 lets them reach the peak, 2 is the most. Same reading rules as `hdr` `mode`. Applies live. |
| `upscale` | `mode` | `off` | `NXRT_UPSCALE_MODE` | `src/gfx/extras/extras_state.cpp` | Upscaling: `off` or `fsr1` (`spatial` is accepted as well). `fsr3`, `dlss` and `xess` are read and kept but not available yet. Play mode reads the `[upscale]` section of `controls.ini` instead; the environment variable overrides the file. Applies live. |
| `upscale` | `sharpness` | `0.8` | `NXRT_UPSCALE_SHARPNESS` | `src/gfx/extras/extras_state.cpp` | FSR 1 sharpening, 0 to 1 (1 is the sharpest). Same reading rules as `upscale` `mode`. Applies live. |
| `upscale` | `scene_scale` | `100` | `NXRT_UPSCALE_SCENE_SCALE` | `src/gfx/play/play_display.cpp` | Percent of the output size per side that the 3D scene is rendered at, 25 to 100 (100 = off). The scene goes through the game's own dynamic-resolution path; the HUD and post effects stay at full size. Same reading rules as `upscale` `mode`. Applies live. |
| `upscale` | `fsr_dll` | empty | `NXRT_UPSCALE_FSR_DLL` | `src/gfx/extras/extras_state.cpp` | Location of `amd_fidelityfx_vk.dll` for the FSR 3 modes; empty means next to the executable. Kept verbatim. Same reading rules as `upscale` `mode`. |
| `upscale` | `dlss_dll` | empty | `NXRT_UPSCALE_DLSS_DLL` | `src/gfx/extras/extras_state.cpp` | Location of `nvngx_dlss.dll` supplied by the player; empty means not used. Kept verbatim. Same reading rules as `upscale` `mode`. |
| `upscale` | `xess_dll` | empty | `NXRT_UPSCALE_XESS_DLL` | `src/gfx/extras/extras_state.cpp` | Location of `libxess.dll` supplied by the player; empty means not used. Kept verbatim. Same reading rules as `upscale` `mode`. |
| `upscale` | `frame_generation` | `0` | `NXRT_UPSCALE_FRAME_GENERATION` | `src/gfx/extras/extras_state.cpp` | `1` turns on FSR 3 frame interpolation (needs the FSR 3 mode, which is not available yet). Same reading rules as `upscale` `mode`. |
| `latency` | `mode` | `off` | `NXRT_LATENCY_MODE` | `src/gfx/extras/extras_state.cpp` | Low latency: `off`, `auto` (whichever of NVIDIA Reflex or AMD Anti-Lag 2 the GPU has), `on` or `boost` (Reflex boost). Play mode reads the `[latency]` section of `controls.ini` instead; the environment variable overrides the file. Applies at the next start. |
| `latency` | `frames_ahead` | `1` | `NXRT_LATENCY_FRAMES_AHEAD` | `src/gfx/extras/lowlat.cpp` | With the mode on, the simulation of a frame starts only when at most this many earlier frames are still on their way to the screen. -1 to 3 (-1 turns the wait off). Same reading rules as `latency` `mode`. Applies at the next start. |
| `latency` | `log_timings` | `0` | `NXRT_LATENCY_LOG_TIMINGS` | `src/gfx/extras/extras_state.cpp` | `1` writes one line of Reflex timings to the log every second. Same reading rules as `latency` `mode`. Applies at the next start. |
| `latency` | `ab_toggle_s` | `0` | `NXRT_LATENCY_AB_TOGGLE_S` | `src/gfx/extras/lowlat.cpp` | Test aid, not a user setting: flips the low-latency mode on and off every N seconds and resets the timing averages, for A/B comparisons. `0`: off. |
| `time` | `mode` | `real` | `NXRT_TIME_MODE` | `src/timestep/timestep.cpp` | Frame step the game runs: `real` (measured frame time, clamped and smoothed), `fixed` (1/`rate` and the frame is held to it: the 60 Hz lock) or `off` (the game's own 1/60 per call). Needs the lifted game built with the S11 sites in `recomp/games/mpr/main.sites`. |
| `time` | `rate` | `60` | `NXRT_TIME_RATE` | `src/timestep/timestep.cpp` | Steps per second in `fixed` mode (10 to 1000). |
| `time` | `max_rate` | `480` | `NXRT_TIME_MAX_RATE` | `src/timestep/timestep.cpp` | `real` mode: the step is never shorter than 1/max_rate, and with `pace` a frame is held until then. This is also the cap of an unlimited frame rate: the game's logic never runs faster than this. |
| `time` | `min_rate` | `30` | `NXRT_TIME_MIN_RATE` | `src/timestep/timestep.cpp` | `real` mode: the step is never longer than 1/min_rate. A slower frame makes the game run in slow motion instead of taking one huge step. |
| `time` | `smooth` | `4` | `NXRT_TIME_SMOOTH` | `src/timestep/timestep.cpp` | `real` mode: the step is the mean of the last N frame times (1 = raw, 64 at most); a frame more than 50 % away from the mean restarts the window. |
| `time` | `pace` | `true` | `NXRT_TIME_PACE` | `src/timestep/timestep.cpp` | Hold the frame in the Update hook until the shortest step the mode allows has passed. |
| `time` | `off_rate` | `0` | `NXRT_TIME_OFF_RATE` | `src/timestep/timestep.cpp` | `off` mode only: hold the frame to this rate and keep the retail 1/60 step. A diagnostic: the game then runs `off_rate / 60` times faster. |
| `time` | `movie_real` | `true` | `NXRT_TIME_MOVIE_REAL` | `src/timestep/timestep.cpp` | The movie list's clock is real elapsed time (up to `movie_max` per call) instead of the game's step. |
| `time` | `movie_max` | `0.5` | `NXRT_TIME_MOVIE_MAX` | `src/timestep/timestep.cpp` | Longest delta handed to the movie list in one call, seconds. |
| `time` | `fix_friction` | `true` | `NXRT_TIME_FIX_FRICTION` | `src/timestep/timestep.cpp` | Scale the in-air walk friction constant (3.5 per 1/60 s) of `CPlayerMP1::GetDampedClampedVelocityWR` by the step. |
| `time` | `fix_render_timings` | `true` | `NXRT_TIME_FIX_RENDER_TIMINGS` | `src/timestep/timestep.cpp` | `NRenderUtilMPT::TickRenderTimings` (texture scroll, UV animation, water) advances by the step instead of one frame per call. |
| `time` | `fix_streaming` | `true` | `NXRT_TIME_FIX_STREAMING` | `src/timestep/timestep.cpp` | Texture streaming ages its entries by the step instead of 1/60. |
| `time` | `fix_budget` | `true` | `NXRT_TIME_FIX_BUDGET` | `src/timestep/timestep.cpp` | The 14 ms background-work budget after the tick scales with the step. |
| `time` | `fix_stuck_tracker` | `true` | `NXRT_TIME_FIX_STUCK_TRACKER` | `src/timestep/timestep.cpp` | The player's stuck tracker samples once per 1/60 s of game time instead of once per tick. Above 60 ticks per second, its 20-sample window was too short, so the top of a jump counted as stuck and the vertical speed was zeroed for one frame. |
| `time` | `log_every` | `0` | `NXRT_TIME_LOG_EVERY` | `src/timestep/timestep.cpp` | One log line of frame statistics every N frames (0 = off). |
| `time` | `csv` | empty | `NXRT_TIME_CSV` | `src/timestep/timestep.cpp` | Write one line per frame: frame, host time, measured elapsed, step, sum of steps, movie frames delivered, render-timing clock. |
| `time` | `game_base` | `0x7100000000` | `NXRT_TIME_GAME_BASE` | `src/timestep/timestep.cpp` | Guest address of the main module (the hooks are keyed by it). |
| `boot` | `gfx` | `window` (`headless` when `[gfx] headless` is set) | `NXRT_BOOT_GFX` | `game-build/src/mpr_main.cpp` | `window`, `headless` or `none`. |
| `boot` | `nvn` | `true` | `NXRT_BOOT_NVN` | `game-build/src/mpr_main.cpp` | `true`: the NVN front end and the graphics device are created. `false` (or `--no-nvn`): neither is. |
| `boot` | `link` | `rtld` | `NXRT_BOOT_LINK` | `game-build/src/mpr_main.cpp` | `rtld`: the game's own loader relocates every module, as on the console. `host`: the runtime's linker does it instead, as a cross-check. |
| `boot` | `probe` | empty | `NXRT_BOOT_PROBE` | `game-build/src/mpr_main.cpp` | `no-sdk-services`: bring-up probe that starts the game without the SDK's service initialisation. Logged as `PROBE`. |
| `boot` | `press` | empty | `NXRT_BOOT_PRESS` | `game-build/src/mpr_main.cpp` | Scripted button presses for player 1, a comma list such as `A@12,A@20:500,PLUS@30,A#1500`. `NAME@seconds` counts from start, `NAME#frame` counts presented frames of the NVN back end, and `:ms` sets the hold (default 300). Names: A, B, X, Y, PLUS, MINUS, UP, DOWN, LEFT, RIGHT, L, R, ZL, ZR. |
| `boot` | `screenshot_at` | `0` | `NXRT_BOOT_SCREENSHOT_AT` | `game-build/src/mpr_main.cpp` | Seconds after start to write `shot_<n>.png` of the presented frame, in the working directory. `0`: off. |
| `boot` | `screenshot_every` | `0` | `NXRT_BOOT_SCREENSHOT_EVERY` | `game-build/src/mpr_main.cpp` | Repeats `screenshot_at` every N seconds. `0`: once only. |
| `boot` | `max_seconds` | `0` | `NXRT_BOOT_MAX_SECONDS` | `game-build/src/mpr_main.cpp` | After this many seconds the process logs a state summary and ends with exit code 124, even if the game is still running. `0`: no limit. `--max-seconds` sets it. |
| `boot` | `main_stack_kb` | `1024` | `NXRT_BOOT_MAIN_STACK_KB` | `game-build/src/mpr_main.cpp` | Stack of the game's main thread, in KiB. |
| `boot` | `dump_killer` | `false` | `NXRT_BOOT_DUMP_KILLER` | `game-build/src/mpr_main.cpp` | Temporary debug hook (debug note 121). Logs two fixed memory slots on an access violation, and the thread stacks when one of them first fills. |
| `boot` | `trace_plt` | `false` | `NXRT_BOOT_TRACE_PLT` | `game-build/src/mpr_main.cpp` | Diagnostic: logs every lazy symbol binding of the game's loader (debug note 018). |
| `time` | `zone` | `host` | `NXRT_TIME_ZONE` | `src/services/time.cpp` | `utc` (any case) sets UTC. Any other value uses the host time zone. |

### Audio notes

When the sink has a clock of its own (XAudio2, or `paced`), the audio renderer keeps about 40 ms of audio queued on it and renders the next 5 ms frame whenever the queue falls below that, instead of waiting on the host clock. The sound card and the host clock never agree exactly; pacing by the host clock alone would fill the queue slowly (frames dropped) or drain it (a click). With the null sink the renderer keeps its 5 ms host-clock pacing. The pace does not depend on the game's frame rate: the renderer runs on its own thread and the game's audio thread waits for the renderer's frame event.

`wav_dump` is read when the first audio sink is created and works with either sink. `device` and `volume` only affect the XAudio2 sink (the null sink plays nothing). `device` is read each time the XAudio2 sink opens. `volume` is read once, at first use, and can also be changed in code while the program runs.

`wav_dump` writes the game's sound to a WAV file as well as to the output device. Every frame that reaches the sink is written, including frames the device drops because its queue is full. The file header is updated about once a second of audio and again when the sink closes, so a file from a run that was killed is still playable up to the last update. If the sample rate or channel count changes during the run (for example a switch from stereo to 5.1), the next file gets a number before its extension: `dump.wav`, then `dump.1.wav`, and so on.

`device` chooses where the XAudio2 sink plays. `default` (or an empty value) uses the system's default output. Any other text is looked for first as an exact endpoint id, then as part of a device's friendly name, ignoring upper and lower case. The first match wins. If nothing matches, the log gets a warning and the default device is used. Each time the XAudio2 sink opens, the log lists the active output devices with their names and ids, so the text for this key can be copied from there.

When a sink closes, it writes one line to the log with its counters: frames written, accepted, dropped, underruns (times the device ran out of audio after playback had started) and the largest number of buffers queued at once.

## Keyboard beam and visor keys (`controls.ini`)

These live in the play-mode file `controls.ini` (key `[play] controls`), not in `nxrt.ini`. The F1 settings page writes the same file.

| Input | Action | What the game does |
|---|---|---|
| `1` | `beam_1` | Power beam |
| `2` | `beam_2` | Wave beam |
| `3` | `beam_3` | Ice beam |
| `4` | `beam_4` | Plasma beam |
| `5` | `visor_1` | Combat visor |
| `6` | `visor_2` | Scan visor |
| `7` | `visor_3` | Thermal visor |
| `8` | `visor_4` | X-ray visor |
| Mouse wheel up / down | `beam_prev` / `beam_next` | Cycle the beams you own, in the order power, wave, ice, plasma. Off when `[mouse] wheel_beams = 0`. |

How it works: the game only knows the Switch pad, so an action is turned into pad buttons by the `[switch]` table. By default `beam_1` to `beam_4` hold X and press the d-pad (up, right, down, left), and `visor_1` to `visor_4` press the d-pad alone (up, right, down, left). The wheel skips beams the player does not have yet. In the menu and map contexts they are switched off (`1` to `8 = none` in `[bindings.menu]` and `[bindings.map]`) and the wheel moves the cursor or the map instead.

To change a key, add a line `Key = action` to `[bindings.all]` (every context) or to one context (`[bindings.gameplay]`, `[bindings.morph]`, `[bindings.scan]`, `[bindings.menu]`, `[bindings.map]`). `Key = none` switches an input off in that context. Key names are SDL scancode names (`Left Ctrl`, `Page Up`, `Mouse1`, `WheelUp`).

`[meta] defaults_rev` records the revision of the factory bindings that the file was written with. The game writes it. A file without the section counts as `0`. When a release adds a new factory binding, the game adds it to an existing `controls.ini` at the next start, but only where that key is still unbound in that context. A key you bound yourself is left alone. A file written before `[meta]` existed has no record of what you removed, so a key you had unbound there gets its new default once. After that, a key you remove stays removed. The first time this happens, the old file is kept next to it as `controls.ini.bak` (an existing `.bak` is never overwritten), and the upgraded file is written at once. The log has one line saying what was added. A file with no `[bindings.*]` section gets the whole factory set and is not rewritten.

`[mouse] menu_cursor`, default `1`: in menus the mouse works like a pointer: a visible cursor, hovering highlights an item, left click activates, right click goes back, the wheel scrolls. While it is on, the left click drives no binding in the menu and map contexts. The old key `menu_touch` is ignored; a file that still has it loads without a warning and nothing changes.

### Lock-on and camera feel (`controls.ini`, `[aim]` and `[camera]`)

`[aim] lock_mode`, default `classic`. `classic` is the game's own lock-on: the view turns to face the target. `free` starts the lock the same way, then centres the view on the target once. After that the mouse and right stick still turn the view, and the view is held so the locked target stays on screen inside the margin. The gun and reticle stay on the target, and shots home and strafing circle it, as in the normal lock. Releasing the lock returns to normal aim. The scan visor always uses the classic lock. See `docs/S32.md` for details.

`[aim] free_lock_margin`, default `0.15`, range `0` to `0.4`: the share of the screen kept clear along every edge while `lock_mode = free` holds the view. `0` lets the target reach the edge; `0.15` keeps it in the middle 70 percent.

`[camera] gun_drives_view`, default `1`. `1` is the game's own behaviour: the arm cannon is fixed to the view, so its idle drift, walking bob and landing dip move the whole view. `0` makes the view follow only the mouse and the stick. Those movements move the gun on screen instead, and the view stays still. Takes effect at once, no restart needed. Set it from the F1 page or the launcher, where it shows as "Steady view" (ticked means `0`).

Checked in a real game run with `tools/test/keys_check.py` (warp to `tallon_canyon_cavern`, grant the other beams and visors, press each key and read the game's own beam and visor numbers): `1` to `4` give power, wave, ice and plasma, the wheel steps wave, ice, wave, and `5` to `8` give combat, scan, thermal and x-ray.

### Controller sticks (`controls.ini`, `[pad]`)

`[pad] deadzone`, default `0.10`, range `0` to `0.5`: the dead zone of the left stick (movement). Before the right stick got its own key, this was the only stick dead zone.

`[pad] right_deadzone`, default `0.10`, range `0` to `0.5`: the dead zone of the right stick (looking). A file with `deadzone` but no `right_deadzone` gives the right stick the value of `deadzone` when read. The next save writes both keys.

`[pad] invert_y`, default `0`: flips the right stick's vertical axis in play, so pushing up looks down. Menus and the map keep the stick as it is. It adds to the game's own invert option in its controls menu, so with both on, looking is back to normal.

`[pad] look_sens`, default `1`, range `0.25` to `3`: multiplies the right stick's deflection in play. Below 1, the top turn speed is lower. Above 1, it is reached sooner. Like `invert_y`, it applies in play only.

`[pad] ball_orbit`, default `2`, range `0` to `6`: how fast, in radians per second, the right stick turns the morph ball camera around the ball at full deflection. `0` turns it off. The mouse still orbits the camera as before.

## Window modes and the picture (`controls.ini`, `[display]`)

`window_mode` is `windowed`, `borderless` or `exclusive`. Borderless covers the display at its desktop mode. Exclusive asks Windows for the desktop mode. If the display refuses (a remote or locked session, some drivers), the game logs "window mode 'exclusive' was refused" and uses borderless instead. The setting in the file stays as it was. Alt+Enter and the F1 page switch modes while playing.

The game's picture size (internal resolution) is fixed when the game starts. It is the size of the window or screen at that moment, unless `render_width` and `render_height`, or `render_scale`, are set. Start in `borderless` on a 4K screen and the game renders at 3840x2160. Start windowed and press Alt+Enter, and the picture keeps the window's size and is scaled up, with bars when `keep_aspect = 1`. For a sharp full-screen picture, set the window mode before starting (the launcher does this), or set `render_width` and `render_height`.

On a picture narrower than 16:9 (16:10, 3:2, 4:3), the HUD and the menus keep their full 16:9 width and show more above and below. Wider pictures keep the HUD at 16:9 in the middle, as before.

The frame rate counter (`[fps] corner`) defaults to `top_left`. The bottom corners overlap the menu prompts and the title screen's copyright line.

New self-test steps (`NXRT_PLAY_SELFTEST`, see `play_selftest.h`). `gate:NAME` holds the script until the file `selftest_NAME` exists in the working folder. The file is then deleted and the script goes on. `report[:label]` logs the window mode, pixel size, focus and frame count.

## Environment variables read directly

These variables are read with `getenv`, outside the key lookup.

| Variable | Read by | Meaning |
|---|---|---|
| `NXRT_INI` | `src/core/runtime.cpp` | Ini file path. Used only when `InitOptions.ini_path` is null. |
| `NXRT_LOG` | `src/diag/log.cpp` | Log file path. Overrides `[log] path`, `NXRT_LOG_PATH` and the path set in code. |
| `NXRT_LOG_LEVEL` | `src/diag/log.cpp` | Log level. Overrides `[log] level`. It is also the variable for that key, listed above. |

## Sample nxrt.ini

Every key at its default value. Lines starting with `;` are comments.

```ini
; nxrt.ini with every key at its default value.
; An environment variable with the same name overrides each value.

[log]
path = nxrt.log
level = info

[memory]
heap_gb = 6
limit_gb = 8
poll_ms = 250

[diag]
watchdog_seconds = 10

[sched]
console_cores = true
host_cpus =

[svc]
unknown_log_limit = 100

[process]
pid = 0x51
entropy_seed = 0x1234567890ABCDEF
system_resource_size = 0
program_id = 0

[threads]
host_stack_mb = 64
[am]
; resolution =  (empty: the default for the operation mode, see CONFIG.md)
operation_mode = docked

[audio]
sink = auto              ; auto (XAudio2, null sink when no device), xaudio2, paced (simulated device), null
; volume = 1.0           ; master volume 0.0 .. 1.0 (XAudio2)
; device = default       ; default, an endpoint id, or part of a device name
; wav_dump = game.wav    ; also write everything the game plays to a WAV file
[media]
movie = mf               ; mf: movies on Media Foundation, off: the library in subsdk0 (no movies)

[title]
program_id = 0x010012101468C000
name = Metroid Prime Remastered
display_version = 1.0.0
; supported_languages =  (empty: the 13 retail languages)
; save_owner_id =  (unset: follows program_id)
account_save_size = 4194304
account_save_journal = 1048576
device_save_size = 0
device_save_journal = 0
cache_save_size = 0
cache_save_journal = 0
main_thread_stack = 1048576
main_thread_priority = 44
main_thread_core = 0

[system]
language = AmericanEnglish
; region =  (empty: USA)
firmware = 12.1.0
nickname = nxrt

[account]
nickname = Samus
; uid =  (empty: the built-in local user id)

[fs]
romfs = game/romfs
; save_dir =  (empty: %APPDATA%\MPRRecomp\save)

[fatal]
; report_path is not listed: once set, even empty, it replaces the default report file.
terminate = true

[net]
offline = true

[pl]
; font_dir =  (empty: the default search folders)

[time]
zone = host
```

## Test tooling keys (`[test]`, work area S5)

All off by default; details in `include/nxrt/tt.h` and `docs/S5.md`. Environment variable = `NXRT_TEST_<KEY>`.

| Key | Default | Meaning |
|---|---|---|
| `script` | empty | Scripted run (`.nxs`, grammar in `include/nxrt/tt_script.h`). Exit code 0 pass, 1 fail, 2 usage/parse error, 4 crash, 5 freeze, 6 process ended early, 124 wall timeout. |
| `enabled` | `false` | Turns the test tools on by themselves, without a script, record, replay, deterministic or trace key set. |
| `regions` | `regions.tsv` next to the script | Name table for `@area` / `@warp` (`name<TAB>world guid<TAB>area guid`). |
| `out` | `testout` | Output folder: `result.json`, `trace.jsonl`, `shots/`. |
| `deterministic` | `false` | Virtual clock (one frame = `clock_step_ticks`), seeded RNG, fixed wall-clock epoch. |
| `seed` | `0x5EED5EED` | RNG seed of deterministic mode. |
| `clock_step_ticks` | `320000` | Virtual ticks (19.2 MHz) per frame. |
| `frame_source` | `hook` | `hook` (needs `tests/sites/mpr_testtools.sites` in the lifted game), `tick` (`diag_frame_tick`), `svc:N` (a frame every N SVCs, bring-up), `manual` (tests). |
| `trace` | `<out>/trace.jsonl` | Per-frame JSON lines; `none` disables. |
| `record` / `replay` | empty | Record the live pad of every frame / feed a recorded input trace. |
| `replay_exit` | `false` | With `replay`: `1` ends the run where the recorded input ends. |
| `hash_regions` | empty | Memory folded into the frame hash: comma list of `0xADDR+0xSIZE` and `module:<name>` (the module's RW pages). |
| `max_frames` | `0` | Fail the run after this many frames. |
| `wall_timeout_s` | `900` with a script | End the run with exit code 124 after this long. |
| `freeze_exit` | `true` with a script | A watchdog fire ends the run with exit code 5. |
| `shot_dir` / `require_shots` | `<out>/shots` / `false` | Where `@shot` writes; a missing screenshot provider fails the run when set. |
| `game_base` | `0x7100000000` | Base of the game's main module (probe and hook addresses). |
| `warp_wait_loader` | `true` | `@warp` waits until the game's resource loader thread has been idle for 10 frames in a row before it starts the warp. Warping while the loader still builds assets for the old game instance crashed it in `CParticleStaticRenderState` (S42). `false` warps at once, as before. |
| `warp_loader_max_ms` | `30000` | How long `@warp` waits for the idle loader, in host milliseconds, before it warps anyway with a warning. |
