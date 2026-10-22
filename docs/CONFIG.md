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
| `memory` | `heap_gb` | `6` | `NXRT_MEMORY_HEAP_GB` | `src/mem/memory.cpp` | Heap section reserve in GiB, from 1 to 1024. Out of range: a warning is logged and 6 is used. Read only when `mem::Config.heap_reserve` is 0. |
| `memory` | `app_memory_mb` | `0` (= 3285, 0xCD500000 bytes) | `NXRT_MEMORY_APP_MEMORY_MB` | `src/kernel/svc_info.cpp`, `svc_memory.cpp` | Physical memory the process is told it has: GetInfo TotalMemorySize, and the ceiling of SetHeapSize together with the module images and 64 MiB. |
| `memory` | `limit_gb` | `8` | `NXRT_MEMORY_LIMIT_GB` | `src/diag/memguard.cpp` | Private commit limit in GB. Zero, negative or non-numeric: 8. |
| `memory` | `poll_ms` | `250` | `NXRT_MEMORY_POLL_MS` | `src/diag/memguard.cpp` | Memory guard poll interval in milliseconds. Zero or negative: 250. |
| `memory` | `fast_query` | `true` | `NXRT_MEMORY_FAST_QUERY` | `src/mem/memory.cpp` | Answer `is_mapped` and `protect` for ranges inside the heap view from a per-page table instead of `VirtualQuery` (which walks every page of the run on a section view: about 75 ms for a 3 GiB heap). `false` goes back to `VirtualQuery`; for before/after measurements. Read once. |
| `diag` | `watchdog_seconds` | `10` | `NXRT_DIAG_WATCHDOG_SECONDS` | `src/diag/watchdog.cpp` | Freeze threshold in seconds. Zero or negative disables the watchdog. Read only when the start call passes no value. |
| `sched` | `console_cores` | `true` | `NXRT_SCHED_CONSOLE_CORES` | `src/kernel/sched.cpp` | `false` turns off the console core model. |
| `sched` | `host_cpus` | empty | `NXRT_SCHED_HOST_CPUS` | `src/kernel/sched.cpp` | Hexadecimal mask of host processors, intersected with the process affinity. Ignored when no bit matches. Empty: the runtime picks processors itself. |
| `sched` | `priority_class` | `auto` | `NXRT_SCHED_PRIORITY_CLASS` | `src/kernel/sched.cpp` | Process priority class: `auto` (high when play mode has a window, otherwise left alone), `high`, `above_normal`, `normal`. The guest threads are pinned with the wake-up boost off, so under background CPU load a woken thread waits a scheduler quantum; see debug note 221. |
| `ipc` | `stub_result` | `0xBCBD5` | `NXRT_IPC_STUB_RESULT` | `src/ipc/stub.cpp` | Result every stub-service command answers. `0` answers success with an empty reply (bring-up probe: the guest then reads zeros, so it only maps which services it asks for next). |
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
| `nvn` | `log_first_calls` | `3` | `NXRT_NVN_LOG_FIRST_CALLS` | `src/gfx/nvn/nvn_loader.cpp` | Calls of each unimplemented NVN entry point logged at Info; later calls are counted and logged at Trace. |
| `nvn` | `unknown` | `stub` | `NXRT_NVN_UNKNOWN` | `src/gfx/nvn/nvn_loader.cpp` | Answer to an NVN name that is not in the table: `stub` hands out a logging stub (logged as UNKNOWN NVN PROC), `null` returns null. |
| `nvn` | `shader_cache` | `` | `NXRT_NVN_SHADER_CACHE` | `src/gfx/nvn/nvn_shaders.cpp` | Folder with `<code hash>.spv` and `results.tsv` (translated shaders). Empty: `./shader_cache/spirv`, then `D:/AI/Active/Metroid-Prime/work/shader_cache/spirv`. |
| `nvn` | `bc7_cache` | `bc7_cache` | `NXRT_NVN_BC7_CACHE` | `src/gfx/nvn/nvn_texdata.cpp` | Folder of ASTC to BC7 transcodes, keyed by a hash of the source bytes. Empty disables. |
| `nvn` | `bc7_quality` | `3` | `NXRT_NVN_BC7_QUALITY` | `src/gfx/nvn/nvn_texdata.cpp` | bc7e preset 0-6 for transcodes done at run time (the setup tool uses 5). |
| `nvn` | `pipeline_cache` | `pipeline_cache.bin` | `NXRT_NVN_PIPELINE_CACHE` | `src/gfx/nvn/nvn_pipeline.cpp` | VkPipelineCache file, loaded with the first pipeline and written by a background thread every 2 s while new pipelines appear (and at shutdown). Empty disables. |
| `nvn` | `pipeline_keys` | `pipeline_keys.bin` | `NXRT_NVN_PIPELINE_KEYS` | `src/gfx/nvn/nvn_prewarm.cpp` | Key file of the pipeline prewarm: every pipeline created at a draw is appended; the next run builds the recorded pipelines of a program on worker threads when the game sets its shaders. Empty disables the prewarm. |
| `nvn` | `prewarm` | `true` | `NXRT_NVN_PREWARM` | `src/gfx/nvn/nvn_prewarm.cpp` | `false` turns the pipeline prewarm (recording and building ahead) off. The pipeline cache file is still used. |
| `nvn` | `prewarm_threads` | `max(1, min(4, cpus / 6))` | `NXRT_NVN_PREWARM_THREADS` | `src/gfx/nvn/nvn_prewarm.cpp` | Worker threads that build recorded pipelines. |
| `nvn` | `pool_prefetch` | `true` | `NXRT_NVN_POOL_PREFETCH` | `src/gfx/nvn/nvn_gpu_pool.cpp` | Import a memory pool into Vulkan on a background thread when a buffer is created over it, instead of on the render thread at the first draw. |
| `prof` | `trace` | empty | `NXRT_PROF_TRACE` | `src/diag/ftrace.cpp` | Frame trace file: zones of the render path, slow SVCs and slow HLE calls with their threads and times, flushed once a second. Analyse with `scripts/prof/ftrace_report.py`. Empty: off (one relaxed load per zone). |
| `prof` | `trace_events` | `6000000` | `NXRT_PROF_TRACE_EVENTS` | `src/diag/ftrace.cpp` | Size of the event array (24 bytes each, allocated when the trace starts); events past it are dropped. |
| `prof` | `stall_ms` | `0` | `NXRT_PROF_STALL_MS` | `src/diag/ftrace.cpp` | With a trace on: when no frame was presented for this many milliseconds, dump every thread's stack to `<trace>.stacks` (up to four times per stall) and, when the stall ends, the CPU time each thread used in it. Analyse with `scripts/prof/stack_report.py`. |
| `am` | `resolution` | empty (docked 1920x1080, handheld 1280x720) | `NXRT_AM_RESOLUTION` | `src/services/am.cpp` | Output size as `WIDTHxHEIGHT`, e.g. `1280x720`. Empty or not parseable: the default for the operation mode. |
| `am` | `operation_mode` | `docked` | `NXRT_AM_OPERATION_MODE` | `src/services/common.cpp` | `handheld` sets handheld mode. Any other value is docked. |
| `audio` | `sink` | `auto` | `NXRT_AUDIO_SINK` | `src/services/audio_sink.cpp` | `auto` plays through XAudio2 on the default output device and falls back to the null sink (no sound, a warning in the log) when XAudio2 or a device is missing. `xaudio2` is the same with an explicit name. `paced` is a simulated device: it drains its queue at the sample rate and reports underruns and queue levels, which is how the renderer's timing is checked on a machine without a sound card. `null` discards everything. Tests install their own sink. |
| `audio` | `paced_ppm` | `0` | `NXRT_AUDIO_PACED_PPM` | `src/services/audio_paced.cpp` | Clock error of the paced sink in parts per million (positive: the simulated device runs fast). Used to check that the renderer follows a device clock that is not exactly the host's. |
| `media` | `movie` | `mf` | `NXRT_MEDIA_MOVIE` | `src/media/movie_mf.cpp` | `mf`: the game's `movie::` imports (Extractor, Decoder, MediaData, Buffer) are answered on Windows Media Foundation. `off`: the library compiled into subsdk0 runs (it needs the NVDEC/VIC driver, which does not exist here, so no movie plays). |
| `media` | `movie_log` | `false` | `NXRT_MEDIA_MOVIE_LOG` | `src/media/movie_mf.cpp` | One log line per sample read and per input buffer sent to a decoder. Very verbose. |
| `audio` | `wav_dump` | empty | `NXRT_AUDIO_WAV_DUMP` | `src/services/audio_sink.cpp`, `src/services/audio_wav.cpp` | Path of a 16-bit PCM WAV file that receives every frame the game sends to the host sink. Empty: no file. See the audio notes below. |
| `audio` | `volume` | `1.0` | `NXRT_AUDIO_VOLUME` | `src/services/audio_sink.cpp`, `src/services/audio_xaudio2.cpp` | Master volume, from `0.0` (silent) to `1.0`. Values outside that range are clamped. Applied by the XAudio2 sink. |
| `audio` | `device` | `default` | `NXRT_AUDIO_DEVICE` | `src/services/audio_xaudio2.cpp`, `src/services/audio_devices.cpp` | Output device for the XAudio2 sink: `default`, an endpoint id, or part of a device's name. See the audio notes below. |
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
| `play` | `selftest` | empty | `NXRT_PLAY_SELFTEST` | `src/gfx/play/play_selftest.h` | Window self-test script for the oracles (steps are listed in that header). |
| `play` | `late_sample` | `true` | `NXRT_PLAY_LATE_SAMPLE` | `src/gfx/play/play.cpp` | Pump the window's events again right before the first `ProcessInput` of a frame takes the mouse counts, so the aim includes what the mouse did since the frame boundary. |
| `hdr` | `mode` | `auto` | `NXRT_HDR_MODE` | `src/gfx/extras/extras_state.cpp` | HDR output: `off`, `auto` (HDR when the display and Windows offer it) or `on`. Play mode reads the `[hdr]` section of `controls.ini` instead; this key only applies to code that does not load that file. The environment variable overrides the file in play mode. Applies at the next start. |
| `hdr` | `format` | `auto` | `NXRT_HDR_FORMAT` | `src/gfx/extras/extras_state.cpp` | Swapchain format: `auto` (HDR10 when offered, else scRGB), `hdr10` or `scrgb`. Same reading rules as `hdr` `mode`. Applies at the next start. |
| `hdr` | `peak_nits` | `0` | `NXRT_HDR_PEAK_NITS` | `src/gfx/extras/extras_state.cpp` | Peak brightness in nits: `0` takes the display's reported peak (1000 when it reports none), otherwise 200 to 10000. Same reading rules as `hdr` `mode`. Applies live. |
| `hdr` | `paper_white_nits` | `0` | `NXRT_HDR_PAPER_WHITE_NITS` | `src/gfx/extras/extras_state.cpp` | Brightness of SDR white in nits: `0` takes Windows' SDR content brightness when HDR is on there (203 otherwise), otherwise 80 to 500. Same reading rules as `hdr` `mode`. Applies live. |
| `hdr` | `highlight_boost` | `1` | `NXRT_HDR_HIGHLIGHT_BOOST` | `src/gfx/extras/extras_state.cpp` | How far highlights go above the game's own white: 0 keeps the SDR look, 1 lets them reach the peak, 2 is the most. Same reading rules as `hdr` `mode`. Applies live. |
| `upscale` | `mode` | `off` | `NXRT_UPSCALE_MODE` | `src/gfx/extras/extras_state.cpp` | Upscaling: `off` or `fsr1` (`spatial` is accepted as well). `fsr3`, `dlss` and `xess` are read and kept but not available yet. Play mode reads the `[upscale]` section of `controls.ini` instead; the environment variable overrides the file. Applies live. |
| `upscale` | `sharpness` | `0.8` | `NXRT_UPSCALE_SHARPNESS` | `src/gfx/extras/extras_state.cpp` | FSR 1 sharpening, 0 to 1 (1 is the sharpest). Same reading rules as `upscale` `mode`. Applies live. |
| `upscale` | `fsr_dll` | empty | `NXRT_UPSCALE_FSR_DLL` | `src/gfx/extras/extras_state.cpp` | Location of `amd_fidelityfx_vk.dll` for the FSR 3 modes; empty means next to the executable. Kept verbatim. Same reading rules as `upscale` `mode`. |
| `upscale` | `dlss_dll` | empty | `NXRT_UPSCALE_DLSS_DLL` | `src/gfx/extras/extras_state.cpp` | Location of `nvngx_dlss.dll` supplied by the player; empty means not used. Kept verbatim. Same reading rules as `upscale` `mode`. |
| `upscale` | `xess_dll` | empty | `NXRT_UPSCALE_XESS_DLL` | `src/gfx/extras/extras_state.cpp` | Location of `libxess.dll` supplied by the player; empty means not used. Kept verbatim. Same reading rules as `upscale` `mode`. |
| `upscale` | `frame_generation` | `0` | `NXRT_UPSCALE_FRAME_GENERATION` | `src/gfx/extras/extras_state.cpp` | `1` turns on FSR 3 frame interpolation (needs the FSR 3 mode, which is not available yet). Same reading rules as `upscale` `mode`. |
| `latency` | `mode` | `off` | `NXRT_LATENCY_MODE` | `src/gfx/extras/extras_state.cpp` | Low latency: `off`, `auto` (whichever of NVIDIA Reflex or AMD Anti-Lag 2 the GPU has), `on` or `boost` (Reflex boost). Play mode reads the `[latency]` section of `controls.ini` instead; the environment variable overrides the file. Applies at the next start. |
| `latency` | `log_timings` | `0` | `NXRT_LATENCY_LOG_TIMINGS` | `src/gfx/extras/extras_state.cpp` | `1` writes one line of Reflex timings to the log every second. Same reading rules as `latency` `mode`. Applies at the next start. |
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
| `time` | `log_every` | `0` | `NXRT_TIME_LOG_EVERY` | `src/timestep/timestep.cpp` | One log line of frame statistics every N frames (0 = off). |
| `time` | `csv` | empty | `NXRT_TIME_CSV` | `src/timestep/timestep.cpp` | Write one line per frame: frame, host time, measured elapsed, step, sum of steps, movie frames delivered, render-timing clock. |
| `time` | `game_base` | `0x7100000000` | `NXRT_TIME_GAME_BASE` | `src/timestep/timestep.cpp` | Guest address of the main module (the hooks are keyed by it). |
| `boot` | `gfx` | `window` (`headless` when `[gfx] headless` is set) | `NXRT_BOOT_GFX` | `game-build/src/mpr_main.cpp` | `window`, `headless` or `none`. |
| `time` | `zone` | `host` | `NXRT_TIME_ZONE` | `src/services/time.cpp` | `utc` (any case) sets UTC. Any other value uses the host time zone. |

### Audio notes

When the sink has a clock of its own (XAudio2, or `paced`), the audio renderer keeps about 40 ms of audio queued on it and renders the next 5 ms frame whenever the queue falls below that, instead of waiting on the host clock. The sound card and the host clock never agree exactly; pacing by the host clock alone would fill the queue slowly (frames dropped) or drain it (a click). With the null sink the renderer keeps its 5 ms host-clock pacing. The pace does not depend on the game's frame rate: the renderer runs on its own thread and the game's audio thread waits for the renderer's frame event.

`wav_dump` is read when the first audio sink is created and works with either sink. `device` and `volume` only affect the XAudio2 sink (the null sink plays nothing). `device` is read each time the XAudio2 sink opens. `volume` is read once, at first use, and can also be changed in code while the program runs.

`wav_dump` writes the game's sound to a WAV file as well as to the output device. Every frame that reaches the sink is written, including frames the device drops because its queue is full. The file header is updated about once a second of audio and again when the sink closes, so a file from a run that was killed is still playable up to the last update. If the sample rate or channel count changes during the run (for example a switch from stereo to 5.1), the next file gets a number before its extension: `dump.wav`, then `dump.1.wav`, and so on.

`device` chooses where the XAudio2 sink plays. `default` (or an empty value) uses the system's default output. Any other text is looked for first as an exact endpoint id, then as part of a device's friendly name, ignoring upper and lower case. The first match wins. If nothing matches, the log gets a warning and the default device is used. Each time the XAudio2 sink opens, the log lists the active output devices with their names and ids, so the text for this key can be copied from there.

When a sink closes, it writes one line to the log with its counters: frames written, accepted, dropped, underruns (times the device ran out of audio after playback had started) and the largest number of buffers queued at once.

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

## Test tooling keys (`[test]`, stream S5)

All off by default; details in `include/nxrt/tt.h` and `docs/S5.md`. Environment variable = `NXRT_TEST_<KEY>`.

| Key | Default | Meaning |
|---|---|---|
| `script` | empty | Scripted run (`.nxs`, grammar in `include/nxrt/tt_script.h`). Exit code 0 pass, 1 fail, 2 usage/parse error, 4 crash, 5 freeze, 6 process ended early, 124 wall timeout. |
| `regions` | `regions.tsv` next to the script | Name table for `@area` / `@warp` (`name<TAB>world guid<TAB>area guid`). |
| `out` | `testout` | Output folder: `result.json`, `trace.jsonl`, `shots/`. |
| `deterministic` | `false` | Virtual clock (one frame = `clock_step_ticks`), seeded RNG, fixed wall-clock epoch. |
| `seed` | `0x5EED5EED` | RNG seed of deterministic mode. |
| `clock_step_ticks` | `320000` | Virtual ticks (19.2 MHz) per frame. |
| `frame_source` | `hook` | `hook` (needs `tests/sites/mpr_testtools.sites` in the lifted game), `tick` (`diag_frame_tick`), `svc:N` (a frame every N SVCs, bring-up), `manual` (tests). |
| `trace` | `<out>/trace.jsonl` | Per-frame JSON lines; `none` disables. |
| `record` / `replay` | empty | Record the live pad of every frame / feed a recorded input trace (`replay_exit=1` ends the run where it ends). |
| `hash_regions` | empty | Memory folded into the frame hash: comma list of `0xADDR+0xSIZE` and `module:<name>` (the module's RW pages). |
| `max_frames` | `0` | Fail the run after this many frames. |
| `wall_timeout_s` | `900` with a script | End the run with exit code 124 after this long. |
| `freeze_exit` | `true` with a script | A watchdog fire ends the run with exit code 5. |
| `shot_dir` / `require_shots` | `<out>/shots` / `false` | Where `@shot` writes; a missing screenshot provider fails the run when set. |
| `game_base` | `0x7100000000` | Base of the game's main module (probe and hook addresses). |
