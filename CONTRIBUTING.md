# Contributing

Thanks for helping. Bug reports with a good log are as useful as code. Small fixes can go straight to a pull request. For anything bigger, open an issue first so we can agree on the approach before you spend time on it.

Problems that only show up in Metroid Prime Remastered itself (a wrong texture, a broken room) belong in the [game repository](https://github.com/drakolordx7/MetroidPrimeRemasteredRecomp). Problems in the recompiler, the runtime or the tools belong here.

## No game data, ever

Do not put any of these in an issue, a pull request, a log excerpt or a commit:

- game dumps (`.nsp`, `.xci`, `.nsz`, `.xcz`), keys (`prod.keys`, `title.keys`) or anything from your console
- files extracted from a game: NSOs, ExeFS, RomFS, textures, models, audio, movies, translated shader caches
- the C++ the recompiler generates from a game, disassembly or decompiled game code, IDA databases

Screenshots and short clips of a bug in a running build are fine. Logs are fine, but skim them before you post. Pull requests that contain game data are closed.

## How the code is laid out

| Part | Where | What it does |
|---|---|---|
| Recompiler | `recomp/` | Turns AArch64 code into C++ that builds natively with clang-cl. Instruction handlers are in `nxrecomp/lift/`, per-game settings in `games/<name>/config.py`. The checks are in `oracle/`: every instruction shape is run in Unicorn and in the generated code and the results must match. |
| Runtime | `runtime/src/` | Memory map and module loader (`mem`, `loader`), threads and the SVC layer (`kernel`), C++ exceptions (`eh`), crash reports, log and freeze watchdog (`diag`), IPC (`ipc`), indirect calls and hook sites (`core`). |
| Services | `runtime/src/services/` | The Horizon system services a game's SDK talks to: file system, saves, account, applet, input, time, audio renderer and more. One file per service group. `runtime/docs/S3c_commands.md` is the command table. |
| Graphics | `runtime/src/gfx/` | A Vulkan 1.3 device layer and renderer. `gfx/nvn/` implements the NVN graphics API on top of it. `gfx/play/` is play mode: window, controls, settings page. |
| Media and timing | `runtime/src/media/`, `runtime/src/timestep/` | Movies on Windows Media Foundation. The frame step for high refresh rates. |
| Tools | `tools/` | Offline tools: Maxwell shaders to SPIR-V (`shaders/`), ASTC to BC7 textures (`textures/`), Tegra block-linear tiling (`tiling/`), the setup tool (`setup/`), the release builder (`release/`), the boot loop (`boot/`), test tools (`test/`) and analysis scripts (`analysis/`). |

Some docs are named `S3c.md`, `S4d.md` and so on. They are development logs, named after the project's work areas: S1 recompiler, S3 OS and services, S4 graphics, S5 test tooling, S6 boot, S8 play mode, S9 movies and audio, S10 display, S11 high refresh.

## Building and testing

You need Windows x64, Visual Studio Build Tools 2026 (default install path), LLVM with clang-cl (23 or newer), CMake, Ninja, sccache, the Vulkan SDK, Python 3.13 with [uv](https://docs.astral.sh/uv/), and for the shader tool the .NET 10 SDK.

```
uv venv tools\.venv --python 3.13
uv pip install --python tools\.venv -r recomp\requirements.txt
uv pip install --python tools\.venv numpy pillow
```

| You changed | Run |
|---|---|
| Runtime | `pwsh runtime\scripts\build.ps1 -Test` (one group: `-Filter <regex>`; one test program directly: `runtime\build\release\nxrt_tests.exe <group>`) |
| Recompiler | From `recomp\`: `python oracle\difftest\difftest.py all`, `python oracle\difftest\mutation.py`, `python oracle\e2e\run_e2e.py`, `python oracle\hooks\test_hooks.py` |
| Test tools | From `tools\test`: `..\.venv\Scripts\python -I -m unittest discover -s tests` |
| Shader, texture or tiling tools | `python tools\<tool>\oracle\run_oracle.py` (see each tool's README) |
| Setup tool | `pwsh -File tools\setup\scripts\build.ps1 -Test` |

Timing tests can fail on a busy machine. Rerun a failing one on its own before you call it broken. `runtime\README.md` has the full test list and `recomp\README.md` explains each oracle.

Folders with an `oracle/LOCKED` file hold hash-locked reference checks (`oracle/SHA256SUMS`). Do not edit them in a pull request. If one is wrong, open an issue and explain why.

## Code style

Match the code around you. There is no formatter.

- C++20, built with clang-cl. Four-space indent, braces on the same line, lines up to about 140 columns. Names are `snake_case` for functions and variables, `kName` for constants, `g_` for globals and `t_` for thread-local state. Logging goes through `NXLOG_E/W/I/D/T`. Settings are read with `config_get_*`, and every new key is added to `runtime/docs/CONFIG.md`.
- Each file starts with a short comment that says what the file is for, and new files carry `SPDX-License-Identifier: GPL-3.0-or-later`. Comments explain why, not what.
- Python is plain and standard-library first, four-space indent, a docstring at the top with the usage, `argparse` for command lines.
- No inline assembly in the recompiler. Its output must be deterministic.
- Fix the mechanism behind a failure. A retry, a sleep or a special case for one asset does not count as a fix.
- Write docs in short, plain sentences.

Code borrowed from other projects keeps its licence notice. Add it to the matching `THIRD_PARTY` file.

## Commits and pull requests

- One logical change per commit. The first line is a plain imperative sentence of about 70 characters or fewer, such as `Fix block-linear untiling for 3D textures`. Use the body to say why. No tool or co-author trailers.
- Say in the pull request what you ran and what it showed. Link the issue it closes. The pull request template has a checklist.
- Keep unrelated clean-ups out of the same pull request.

## Reporting a bug with a log

Use the issue forms and attach what you have.

- Your GPU, driver version and Windows version, and the exact command you ran.
- The log file. The runtime writes `nxrt.log` in the working directory by default (`[log] path`, `NXRT_LOG`). A game built on it may name it differently, for example `mpr.log`. The previous run is kept as `<name>.prev`.
- For a crash, the crash report: `<program name>_crash.txt` next to the executable. For a fatal service error, `<program name>_fatal.txt`. For a freeze, `<program name>_freeze.txt`, which the watchdog writes when the program stops making progress for 10 seconds.
- A screenshot of the bug in a running build.

For more detail, turn on the trace switches. Each is a key in the ini file (`nxrt.ini`, or the game's own) or an environment variable (`[section] name` becomes `NXRT_SECTION_NAME`). All keys are listed in [`runtime/docs/CONFIG.md`](runtime/docs/CONFIG.md).

| Switch | What it adds |
|---|---|
| `[log] level = trace` (`NXRT_LOG_LEVEL=trace`) | Every log line down to trace level. Valid values are `error`, `warn`, `info`, `debug` and `trace`. |
| `[hle] trace = nvn` (`NXRT_HLE_TRACE=nvn`) | An ordered log of every host-side call whose name starts with the prefix, with arguments. `nvn` shows the graphics API calls. `trace_limit` caps the lines (default 20000). |
| `[nvn] trace_frame = N` (`NXRT_NVN_TRACE_FRAME=N`) | Every draw, clear and copy of presented frame N, with shaders, render targets and state. The other `[nvn] trace_*` keys are in [`runtime/docs/S4e.md`](runtime/docs/S4e.md). |
| `[gfx] validation = true` (`NXRT_GFX_VALIDATION=1`) | Vulkan validation layer messages. Needs the Vulkan SDK. |

Trace output can be large. Trim it to the part around the bug, or attach it as a file.
