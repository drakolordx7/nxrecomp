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

