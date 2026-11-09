# Building

The base builds on its own. A game is a folder (`games/<id>`) that the base takes as input: its manifest `game.json` names the game,
its builds, modules and files; `recomp/` holds its lift config and sites, `runtime/` its runtime module and tests, `game-build/` the
CMake project of its exe.

Windows x64, clang-cl + Ninja + sccache (the scripts call `runtime\scripts\env.cmd` for the MSVC environment). Vulkan tools are found
through `VULKAN_SDK`, which the Vulkan SDK installer sets.

## Folders

| Name | What | Default |
| --- | --- | --- |
| `NX_ROOT` | the folder holding `games\` and `build\` | env `NX_ROOT`, else the nearest parent of the checkout with a `games\` folder |
| `NX_BASE` | the base checkout a game builds against | `<game>\base` when it holds `runtime\CMakeLists.txt`, else `<game>\..\..\base` |
| build output | one folder per checkout | `NX\build\<checkout folder name>\...` (a worktree `NX\wt\base-x` builds into `NX\build\base-x`) |
| lifted libraries | `<id>_<module>.lib` | `games\<id>\work\build\<id>`: kept there, a game exe build only imports them |

## Base: runtime and its tests

```
pwsh base\runtime\scripts\build.ps1 -Test [-A2B <astc2bc7 build dir>]
```

Output: `NX\build\base\runtime\release`. The tests register the neutral fixture game (`runtime\tests\fixture_game`). `-A2B` points at a
built `tools\textures` (default `NX\build\textures`, else `tools\textures\build\release`). `-Games "<dir>;<dir>"` (CMake
`NXRT_GAME_DIRS`) also builds those games' runtime modules and their tests (`mpr_core_tests`, ...). One test group:
`pwsh base\runtime\scripts\t.ps1 svc_fs`.

## A game: lift, then the exe

1. Lift (only when the lifter, the config or the sites change; it takes a long time):

   ```
   base\recomp\scripts\build_game.cmd games\<id>
   ```

   Reads `games\<id>\game.json` (modules, bases, shard counts, sites) and `recomp\config.py`, the NSOs from `games\<id>\game\exefs`, and
   writes `games\<id>\work\build\<id>\<id>_<module>.lib`. `NX_BUILD` overrides the build folder. A build folder configured before the
   untangle (its CMakeCache points at `recomp/games/<id>`) must be replaced: set `NX_BUILD` to a new folder or delete its CMakeCache.txt.

2. The exe and the launcher, against the libraries already there:

   ```
   pwsh games\<id>\game-build\scripts\build.ps1 [-Base <base>] [-LibDir <dir>] [-Out <dir>] [-Release] [-Version 0.1.0]
   ```

   Output: `NX\build\<id>\release\<id>.exe` (dev build: reads `games\<id>\game\exefs` and `romfs`). `-Release` builds `<id>.exe` and
   `<id>_launcher.exe` into `...\relpkg` (GUI program, `.\exefs`, no build paths). The CMake function is
   `nxrt_add_game_exe(<id> GAME_DIR <dir> LIB_DIR <dir> VERSION <v>)` (`runtime\cmake\game_exe.cmake`); the exe is the base's
   `runtime\apps\game\game_main.cpp`.

## Setup tool and release

```
pwsh base\tools\setup\scripts\build.ps1 -Game games\<id> [-Test] [-Version 0.1.0]
python base\tools\release\make_release.py --game games\<id> --exe NX\build\<id>\relpkg\<id>.exe --setup-bin NX\build\setup-<id>\bin
```

The setup tool is built for one game (CMake `NX_GAME_DIR`): its profile is generated from the manifest. The release takes every file
name from the manifest's `files` and `release` sections.
