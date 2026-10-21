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
| `memory` | `limit_gb` | `8` | `NXRT_MEMORY_LIMIT_GB` | `src/diag/memguard.cpp` | Private commit limit in GB. Zero, negative or non-numeric: 8. |
| `memory` | `poll_ms` | `250` | `NXRT_MEMORY_POLL_MS` | `src/diag/memguard.cpp` | Memory guard poll interval in milliseconds. Zero or negative: 250. |
| `diag` | `watchdog_seconds` | `10` | `NXRT_DIAG_WATCHDOG_SECONDS` | `src/diag/watchdog.cpp` | Freeze threshold in seconds. Zero or negative disables the watchdog. Read only when the start call passes no value. |
| `sched` | `console_cores` | `true` | `NXRT_SCHED_CONSOLE_CORES` | `src/kernel/sched.cpp` | `false` turns off the console core model. |
| `sched` | `host_cpus` | empty | `NXRT_SCHED_HOST_CPUS` | `src/kernel/sched.cpp` | Hexadecimal mask of host processors, intersected with the process affinity. Ignored when no bit matches. Empty: the runtime picks processors itself. |
| `svc` | `unknown_log_limit` | `100` | `NXRT_SVC_UNKNOWN_LOG_LIMIT` | `src/kernel/svc.cpp` | Each unknown SVC number is logged for its first N calls, then every 1000th call. Read on the first unknown SVC. |
| `process` | `entropy_seed` | `0x1234567890ABCDEF` | `NXRT_PROCESS_ENTROPY_SEED` | `src/kernel/svc_info.cpp` | Seed for GetInfo RandomEntropy, subtypes 0 to 3. |
| `process` | `system_resource_size` | `0` | `NXRT_PROCESS_SYSTEM_RESOURCE_SIZE` | `src/kernel/svc_info.cpp` | Value returned by GetInfo SystemResourceSize. |
| `process` | `program_id` | `0` | `NXRT_PROCESS_PROGRAM_ID` | `src/kernel/svc_info.cpp` | Value returned by GetInfo ProgramId. |
| `process` | `pid` | `0x51` | `NXRT_PROCESS_PID` | `src/kernel/svc_info.cpp` | Process id returned by GetProcessId. The InitialProcessIdRange query uses the built-in `0x51` and ignores this key. |
| `threads` | `host_stack_mb` | `64` | `NXRT_THREADS_HOST_STACK_MB` | `src/kernel/thread.cpp` | Host stack reservation per guest thread, in MiB. Values below 1: 64. |

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
```
