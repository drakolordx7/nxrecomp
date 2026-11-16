# Building

The base builds on its own. A game is a folder (`games/<id>`) that the base takes as input: its manifest `game.json` names the game,
its builds, modules and files; `recomp/` holds its lift config and sites, `runtime/` its runtime module and tests. The exe's CMake project
is the base's (`runtime/cmake/game_project`), so a game folder holds nothing else to build.

Windows x64, clang-cl + Ninja + sccache. Every script gets its environment from `scripts\env.cmd` of the base (MSVC through vswhere,
LLVM, CMake, Ninja, Python, `NX_ROOT`, `NX_BASE`, `NX_CHECKOUT`, `NX_CHECKOUT_NAME`); PowerShell scripts dot-source `scripts\nxpaths.ps1`, Python scripts import `scripts/nxpaths.py`,
CMake reads `cmake/nx_game.cmake`: one rule for each. `LLVM_BIN`, `NX_PYTHON` and `NX_JOBS` override the defaults. Vulkan tools are found
through `VULKAN_SDK`, which the Vulkan SDK installer sets.

## Folders

| Name | What | Default |
| --- | --- | --- |
| checkout | the whole tree (`base\`, `games\`): the main checkout `NX`, or a worktree `NX\wt\<name>` (`git worktree add NX\wt\<name> -b <branch>`) | the parent of `NX_BASE` (`NX_CHECKOUT`) |
| `NX_ROOT` | the main checkout: it holds the local-only game data (`games\<id>\game`, `work`), `build\`, `wt\` | env `NX_ROOT`, else the parent of `wt\` when the checkout is a worktree, else the checkout |
| `NX_BASE` | the base folder whose scripts build: `<checkout>\base` | env `NX_BASE`, else the folder holding the script |
| build output | one folder per checkout, `NX_CHECKOUT_NAME` | `NX\build\main\...` for the main checkout, `NX\build\<name>\...` for the worktree `NX\wt\<name>`. Keep `textures`, `publish`, `setup-*` as non-worktree names |
| game data | `games\<id>\game`, `games\<id>\work` | only in the main checkout; a worktree's builds and runs read them from `NX_ROOT\games\<id>` |
| lifted libraries | `<id>_<module>.lib` | `NX\games\<id>\work\build\<id>`: kept there, a game exe build only imports them (a worktree's own lift: `...\<id>_<name>`) |

## Base: runtime and its tests

```
pwsh base\runtime\scripts\build.ps1 -Test [-A2B <astc2bc7 build dir>]
```

Output: `NX\build\main\runtime\release` (a worktree `x`: `NX\build\x\runtime\release`). The tests register the neutral fixture game (`runtime\tests\fixture_game`). `-A2B` points at a
built `tools\textures` (default `NX\build\textures`, else `tools\textures\build\release`). `-Games "<dir>;<dir>"` (CMake
`NXRT_GAME_DIRS`) also builds those games' runtime modules and their tests (`mpr_core_tests`, ...). One test group:
`pwsh base\runtime\scripts\t.ps1 svc_fs`.

## A game: lift, then the exe

1. Lift (only when the lifter, the config or the sites change; it takes a long time):

   ```
   base\recomp\scripts\build_game.cmd games\<id>
   ```

   Reads `games\<id>\game.json` (modules, bases, shard counts, sites) and `recomp\config.py`, the NSOs from `games\<id>\game\exefs`, and
   writes `games\<id>\work\build\<id>\<id>_<module>.lib`. `NX_BUILD` overrides the build folder; the data is `games\<id>` of the main checkout when the game folder holds no `work\` (a worktree). A build folder configured before the
   untangle (its CMakeCache points at `recomp/games/<id>`) must be replaced: set `NX_BUILD` to a new folder or delete its CMakeCache.txt.

2. The exe and the launcher, against the libraries already there:

   ```
   pwsh base\runtime\scripts\build_game_exe.ps1 -Game games\<id> [-LibDir <dir>] [-Out <dir>] [-Release] [-Version 0.1.0]
   ```

   Output: `NX\build\main\<id>\release\<id>.exe` (a worktree `x`: `NX\build\x\<id>\release`; dev build: reads `games\<id>\game\exefs` and `romfs`). `-Release` builds `<id>.exe` and
   `<id>_launcher.exe` into `...\relpkg` (GUI program, `.\exefs`, no build paths). The CMake function is
   `nxrt_add_game_exe(<id> GAME_DIR <dir> LIB_DIR <dir> VERSION <v>)` (`runtime\cmake\game_exe.cmake`, called by `runtime\cmake\game_project`); the exe is the base's
   `runtime\apps\game\game_main.cpp`.

## Setup tool and release

```
pwsh base\tools\setup\scripts\build.ps1 -Game games\<id> [-Test] [-Version 0.1.0]
python base\tools\release\make_release.py --game games\<id> --exe NX\build\main\<id>\relpkg\<id>.exe --setup-bin NX\build\setup-<id>\bin
```

The setup tool is built for one game (CMake `NX_GAME_DIR`): its profile is generated from the manifest. The release takes every file
name from the manifest's `files` and `release` sections.

The manifest's optional `setup_hook` names a CMake file in the game folder (Metroid: `setup/hook.cmake`) that adds the game's part of the setup
tool (texture list, shader program scan, ini lines); without it the tool only extracts. The file formats the runtime and the setup tool share
(BC7 cache key, `programs.bin`, shader pack) are `formats/nx_formats.h` and its Python twin, checked by `python formats/test_formats.py`.

## Linux x86-64

One codebase for both systems: `cmake/nx_platform.cmake` is the one place that decides the platform (`NX_PLATFORM` win32|linux) and holds every
flag, library name and source folder that differs; a build rule is fixed once. Platform code lives in `runtime/src/os/win32` and
`runtime/src/os/posix` (see `runtime/include/nxrt/os/README.md`), picked up by a glob.

Toolchain: clang (23), lld, Ninja, CMake 3.25+ (`cmake/linux-clang.cmake`; `NX_CLANG_VERSION` picks another LLVM version). SDL3 is built from
source (statically): X11, Wayland, PipeWire, Pulse and ALSA development packages (and `libxtst-dev`) must be installed. The ASTC transcoder
(`tools/textures`) and with it the runtime's gfx and the setup tool need Intel `ispc` (BSD-3, v1.32.0 like the Windows copy): on PATH, or in
`NX/toolchain/ispc/bin`, or `--ispc FILE` / env `NX_ISPC` (`NX/build/textures/linux` also works for the runtime: a prebuilt `libastc2bc7.a`).

### One command: `wt.py`

The Linux counterpart of `wt.ps1` is `base/scripts/nx_build.py` (all the logic, Python 3 stdlib only); `wt.py` is the entry point on both
systems and takes the same options. In WSL (distro `nx-linux`, NX at `/mnt/d/...`) or from Windows with `--linux`:

```
python3 wt.py <name> [--game <id>] [--libs] [--test <ctest regex>] [--no-exe] [--setup] [--release --version V] [--clean] [--dry-run]
python  wt.py <name> --linux ...        (Windows: runs the same inside WSL; env NX_WSL_DISTRO, default nx-linux)
pwsh    wt.ps1 <name> -Linux ...        (the same, from the stream script)
```

| What | Where |
| --- | --- |
| runtime + tests | `NX/build/<name>/linux/runtime` (`cmake --preset linux-release`, then `ninja -k 0`; `--test .` runs ctest) |
| game exe | `NX/build/<name>/linux/<id>/<files.exe without .exe>` (`<id>_relpkg` with `--release`: `NXRT_RELEASE=ON`, FFmpeg bundled) |
| lifted libraries | `NX/games/<id>/work/build/linux/<id>[_<name>]` (`lib<id>_<module>.a`; `--libs` lifts them, generated sources in `..._gen`) |
| setup tool | `NX/build/setup-<id>-linux/bin` (`--setup`; the CLI, the GUI and the tests) |
| runs | `games/<id>/work/run_linux[_<name>]` (`boot.py --game <id>` on Linux: same options and test-run rules; `playtest_linux`) |

`<name>` is a worktree (`NX/wt/<name>`) or `main`; the presets of the three CMake projects (`runtime`, `runtime/cmake/game_project`,
`recomp/game`, `tools/setup`) set the toolchain and are meant to be used with `-B <dir>` as above. The launcher and the setup window are the same SDL3 + Dear ImGui programs on both systems (below).
FFmpeg (movies; `runtime/cmake/ffmpeg.cmake`): Windows fetches the pinned BtbN LGPL shared build into the archive cache and
copies its DLLs next to every exe; Linux uses the system packages (`libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev`,
FFmpeg 4.4 or newer), or `-DNXRT_FFMPEG_BUNDLED=ON` fetches the same pinned build as `.so` files that are copied next to the exe ($ORIGIN rpath;
what a release uses). `-DNXRT_FFMPEG_DIR=<prefix>` takes any other build. The setup tool: (`python3 wt.py main --game <id> --setup --test`, 14 of 16 test groups
need nothing else: `shadercache` wants the Python module `zstandard`, `texcache` a real ispc build).

### Setup tool and release on Linux

`<id>_setup_cli` builds and runs on Linux (`fileio`, `keys`, `log`, `shadercache` and the CLI use `std::filesystem` and POSIX calls behind
`_WIN32` checks; keys are looked up in `~/.config` and `~/.local/share` of yuzu, Eden, suyu, Sudachi, Citron and Ryujinx, `~/.switch`, next to the
program and in the current folder).

```
python tools/release/make_release.py --game games/<id> --platform linux-x86_64 --exe NX/build/<name>/linux/<id>_relpkg/<id> --setup-bin NX/build/setup-<id>-linux/bin
bash release.sh <game> <version> --linux        (builds in WSL, packs the zip and the tarball side by side)
```

`<id>-<version>-linux-x86_64.tar.xz` holds one folder with: the exe, `libastcdec-{avx2,sse4.1,sse2}-shared.so`, the five FFmpeg `.so` files
(`libavcodec.so.62` ...), `<id>_setup` (the setup window), `<id>_setup_cli`, `<id>_launcher` (the launcher), all three the same SDL3 + Dear ImGui programs as on Windows,
the shader pack, the templates, `LICENSE`, `THIRD_PARTY_NOTICES.txt`, `licenses/`, `fonts/`, `prompts/` and `MANIFEST.sha256`. SDL3 is linked into
each program (static), so no SDL file ships; `make_release.py` refuses a release without the launcher or the setup window. The ELF audit (`make_release.py`): x86-64 only; `DT_NEEDED` limited to glibc (`libc`, `libm`, `libdl`,
`libpthread`, `librt`, `libutil`, the loader), `libvulkan`, the X11/xcb/Wayland/xkbcommon client libraries and the libraries shipped beside it
(`libstdc++` and `libgcc_s` are refused: they are linked statically); RPATH/RUNPATH only `$ORIGIN`; glibc symbol versions up to 2.35
(`MAX_GLIBC`, the build box); no local build paths in the programs (WSL spelling included).

Windows releases ship the five FFmpeg DLLs next to the exe. They are built with mingw against the UCRT, so the CRT-import rule that protects
our own binaries (`vcruntime*`, `msvcp*`, `ucrtbase`, `api-ms-win-crt-*` refused) would refuse them; `tools/release/ffmpeg_pins.json` lists them by
file name and SHA-256 and only a file that matches both is let through. Updating FFmpeg means changing `runtime/cmake/ffmpeg.cmake` and that file
together (a test compares the archive hashes). The Linux `.so` hashes are `null` until the first bundled build is measured
(`make_release.py --print-ffmpeg-pins <folder>`); until then they pass through the same ELF audit as our own files.

The merge batch check (`bash batch_test.sh <game>`) ends with a Linux pass (`linux_check.py`): runtime, tests, game module and exe
built in `nx-linux`, ctest, a headless boot when the exe exists; it prints `LINUX OK`, `LINUX SUSPECT` or `LINUX SKIPPED` with the reason
(a Linux build that does not link yet is SKIPPED with its first errors).

### Launcher and setup window

The launcher (`NXRT_BUILD_LAUNCHER`, on by default) is the same SDL3 + Dear ImGui program on both systems (`base/ui` is the shell, `runtime/apps/launcher` the pages; the setup window `tools/setup/gui/main_ui.cpp` uses the same shell). With `NXUI_SHOT=<file.bmp>` set, either program saves its window after a few frames and exits (a test aid; `SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software` runs it headless).

Test and automation aids (the same on both systems; the scripts `games/mpr/launcher_e2e.py` and `cleanroom_rc.py` use them, no window messages are involved): `<id>_launcher --auto-save` loads `controls.ini` through the pages and saves it back without a window; `--auto-report <file>` writes the folder state (`state=ready|not_ready`, `summary`, `settings`, `pages`, ...) as `key=value` lines without a window; `--auto-press setup|play` presses that button on the first frame; `--page <n>` opens a tab; `--game`, `--setup-args`, `--game-args`, `--auto-play`, `--auto-prepare` and `--no-prepare` as before. The setup window takes `--auto-start --exit-when-done --no-message-box`.

The ASTC transcoder (`tools/textures`) needs `ispc` on PATH on Linux, or a prebuilt copy in `NX/build/textures/linux`.

