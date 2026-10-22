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
| `diag` | `watchdog_seconds` | `10` | `NXRT_DIAG_WATCHDOG_SECONDS` | `src/diag/watchdog.cpp` | Freeze threshold in seconds. Zero or negative disables the watchdog. Read only when the start call passes no value. |
| `sched` | `console_cores` | `true` | `NXRT_SCHED_CONSOLE_CORES` | `src/kernel/sched.cpp` | `false` turns off the console core model. |
| `sched` | `host_cpus` | empty | `NXRT_SCHED_HOST_CPUS` | `src/kernel/sched.cpp` | Hexadecimal mask of host processors, intersected with the process affinity. Ignored when no bit matches. Empty: the runtime picks processors itself. |
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
| `am` | `resolution` | empty (docked 1920x1080, handheld 1280x720) | `NXRT_AM_RESOLUTION` | `src/services/am.cpp` | Output size as `WIDTHxHEIGHT`, e.g. `1280x720`. Empty or not parseable: the default for the operation mode. |
| `am` | `operation_mode` | `docked` | `NXRT_AM_OPERATION_MODE` | `src/services/common.cpp` | `handheld` sets handheld mode. Any other value is docked. |
| `audio` | `sink` | `null` | `NXRT_AUDIO_SINK` | `src/services/audio_sink.cpp` | `xaudio2` plays through XAudio2. Any other value keeps the null sink (no output). |
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
| `time` | `zone` | `host` | `NXRT_TIME_ZONE` | `src/services/time.cpp` | `utc` (any case) sets UTC. Any other value uses the host time zone. |

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
sink = null

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
