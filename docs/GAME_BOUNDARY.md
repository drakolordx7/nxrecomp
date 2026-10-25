# The game boundary

The runtime, the recompiler and the offline tools were written for one game, Metroid Prime Remastered 1.0.0. This file lists every
place in them that knows about that game, says what kind of knowledge it is, and says where it lives now. The goal is that a second
Switch title can reuse the base unchanged and bring its own module.

Three kinds of game knowledge:

- **(a) A generic thing with a Metroid value baked in.** The code works for any game; only a number or a name was the game's.
  The value now comes from the game (a field of `nxrt::game::Info`, or a per-game data file in the tools).
- **(b) A reusable mechanism with Metroid data.** The code is a general technique (patching the frame step at a list of sites,
  matching button icons in the game's textures, factory settings with revisions); the table it works from was the game's. The
  mechanism stays in the base and the table moved to the game module.
- **(c) Purely game-specific.** Hooks on Metroid functions, Metroid camera and aim behaviour, its menus. Moved whole into the game
  module.

Scope: `runtime/` (base and launcher), `tools/` (setup tool, release builder) and `recomp/` outside `recomp/games/mpr`, which was
already the recompiler's game folder. The game exe (`game-build/`, root repository) is the game's own program and stays there.

## The interface

`include/nxrt/game.h` holds one struct, `nxrt::game::Info`, and a registry with one slot: `set_current(const Info*)`, `current()`
and `info()` (an empty Info when nothing is registered, so the base runs without a game and the hooks that need one are skipped with
a log line). It is header-only, so the launcher can use it without linking the runtime.

`Info` has:

| Field | Kind | What the base does with it |
| --- | --- | --- |
| `id`, `title`, `product`, `program_id`, `version`, `build_id`, `main_base` | identity | logs, window titles, the launcher's branding |
| `exe_name`, `setup_exe_name`, `launcher_ini`, `game_ini`, `shader_pack`, `save_folder` | file names | the launcher finds and starts the programs; services put saves under `%APPDATA%\<save_folder>` |
| `title_defaults(TitleProfile&)` | (a) | the services' title profile (program id, name, version, languages, save sizes, main thread, nickname, user id) |
| `timestep` (`timestep::Sites`) | (b) | the frame-step patch: frame update entry, mid-function sites with their load kind, render timing counter and its wrap, movie update, per-tick sampler |
| `probes` (`tt::GameProbes`) | (c) behind a generic API | test tooling: state readout, warp, grant, item, guest calls, object dump, ray casts, watches |
| `play_defaults` (`play::GameDefaults`) | (b) | factory `controls.ini`, the defaults revision and the bindings added by each revision |
| `play` (`play::GamePlay`) | (c) behind a generic API | play mode's game half: input and camera hooks, display hooks, menus, status |
| `prompts` (`play::PromptGame`) | (b) | button prompt icons: the table of icons, how to scan the game's textures, icon size, orientation |

A game module registers its Info before `nxrt::init()`. The Metroid module is `runtime/games/mpr` and comes as three libraries, each
a superset of the one before, so each program links only what it needs:

| Library | Adds | Registered by | Linked by |
| --- | --- | --- | --- |
| `nxrt_game_mpr_data` | identity, names, factory settings (pure data) | `nxrt_register_game_mpr_data()` | the launcher and its tests |
| `nxrt_game_mpr_core` | title profile, timestep sites, test probes (links nxrt) | `nxrt_register_game_mpr_core()` | `nxrt_tests` |
| `nxrt_game_mpr` | play-mode hooks, prompts (links nxrt_gfx and the pak reader) | `nxrt_register_game_mpr()` | `mpr.exe`, `nxrt_gfx_tests` |

The play-mode half talks to the base through two tables in `src/gfx/play/play_game.h`. `GamePlay` is what the game gives the base
(function pointers: `install_render`, `install`, `remove`, `settings_changed`, `frame_begin`, `frame`, `display_live`, `menu_*`,
`status`, `status_text`, `mouse_tab`, plus the guest address of the frame update hook). `GameHost` is what the base gives the game
(a virtual class implemented in `play.cpp`: current settings, action state, the right stick, pad look, the per-input-tick mouse
handling, aim feedback, pitch limit, log status). Hooks keep their registration order: the base installs the game's render hooks
before its own frame hook and the game's input hooks after it, as before.

CMake: `NXRT_BUILD_GAMES` (on) adds `games/mpr`. `NXRT_TEST_GAME` (mpr) picks the module the test programs register;
`NXRT_LAUNCHER_GAME` (mpr) picks the launcher's branding and its `games/<id>/launcher.rc`. The setup tool has `MPRS_GAME` (mpr) and
the release builder reads `tools/release/games/<id>.json` (`MPR_RELEASE_GAME`, default mpr).

## The audit

### runtime: services

| Where (before) | Kind | Now |
| --- | --- | --- |
| `include/nxrt/services.h` TitleProfile defaults: program id 0x010012101468C000, "Metroid Prime Remastered", "1.0.0", owner, account save 4 MiB / journal 1 MiB, nickname "Samus", user id "MPRRecom" | (a) | neutral defaults in the base (0, "", "Player", "nxrtuser"); the Metroid values in `games/mpr/mpr_core.cpp` `title_defaults` |
| `src/services/common.cpp` default language mask (13 languages) | (a) | `title_defaults`; the base falls back to American English |
| `src/services/common.cpp` main thread stack 0x100000, priority 44, core 0 | (a) | `title_defaults` |
| `src/services/common.cpp` save folder `%APPDATA%\MPRRecomp` | (a) | `Info::save_folder` (fallback "nxrt") |

### runtime: frame step (timestep)

| Where (before) | Kind | Now |
| --- | --- | --- |
| `include/nxrt/timestep.h` `namespace site`: frame update, the dt clamp/loop/floor sites, four budget sites, friction, two pairs of stream sites, render timings, movie update | (b) | mechanism in `src/timestep/timestep.cpp`, which walks a `Sites` table of `MidSite{offset, load, reg, value}`; the Metroid table in `games/mpr/mpr_core.cpp`, the constants in `games/mpr/mpr_sites.h` |
| `kGameBudget` 0.014, friction 3.5 per tick | (a) | `MidSite::value` of those sites |
| `kRenderCounterWrap` 53000 | (a) | `Sites::render_counter_wrap` |

### runtime: play mode

| Where (before) | Kind | Now |
| --- | --- | --- |
| `src/gfx/play/play.cpp` ProcessInput, SetTransform, first-person and ball camera think hooks, player pointer, free look, ball look, beam hints | (c) | `games/mpr/mpr_play.cpp` behind `GamePlay`; the mouse sampling logic that was inside the input hook stays in the base as `GameHost::input_tick` |
| `src/gfx/play/play_aim.*`, `play_lockcam.*`, `play_lookrate.*` (Samus's aim, lock-on camera, look rate) | (c) | `games/mpr/` (git mv) |
| `src/gfx/play/play_display.cpp` the hooks on the game's resolution, FOV and HUD code | (c) | `games/mpr/display_hooks.cpp`; the display math and the texture filtering settings (`display_tuning`) stay in the base |
| `src/gfx/play/menu_mouse.cpp` the game's menu hooks | (c) | `games/mpr/menu_mouse_game.cpp`; the pointer state and the math stay in the base (`menu_mouse_math.cpp`) |
| `src/gfx/extras/tonemap_hook.*` | (c) | `games/mpr/mpr_tonemap.*` |
| `src/gfx/play/play_ui.cpp` F1 mouse tab: morph ball sliders, wheel cycles beams, lock-on settings | (c) | `games/mpr/mpr_ui.cpp`, called through `GamePlay::mouse_tab` at the same three places |
| `src/gfx/play/play_defaults.cpp` factory controls.ini, defaults revision 2, the BtnX map binding added in revision 2 | (b) | revision mechanism in `play_config.cpp`; the data in `games/mpr/mpr_defaults.cpp` (`GameDefaults`) |
| `src/gfx/play/prompts.cpp`, `prompt_resolve.cpp` the icon table and the Retro pak/TXTR scan | (b) | matching and drawing stay in the base; `games/mpr/mpr_prompts.cpp` gives the table, the scan, the icon size (36) and orientation |
| `cmake/gfx.cmake` `nxrt_mprpak` (the setup tool's pak reader) linked into nxrt_gfx | (c) | built and linked by `games/mpr/CMakeLists.txt` only |

### runtime: test tooling

| Where (before) | Kind | Now |
| --- | --- | --- |
| `src/testtools/tt_probe.cpp` state readout, warp, grant, give item, guest calls, object dump, probe positions, ray casts, watches | (c) | `games/mpr/mpr_tt_probe.cpp` (namespace `nxrt::tt::mpr`), registered as `tt::GameProbes`; `src/testtools/tt_game.cpp` keeps the base address, memory access, `@peek`/`@poke` and forwarders |

### runtime: launcher

| Where (before) | Kind | Now |
| --- | --- | --- |
| "Metroid Prime Remastered PC" window title and dialog captions | (a) | `Info::product` |
| `mpr.exe`, `mpr_setup.exe`, `mpr_launcher.ini`, `shader_cache.mprshc` | (a) | `Info` file names |
| `apps/launcher/launcher.rc`, `launcher.manifest` (version info, icon, manifest) | (c) | `games/mpr/` (git mv), picked by `NXRT_LAUNCHER_GAME` |

### game exe (root repository)

`game-build/src/mpr_main.cpp` is the game's own main. It now calls `nxrt_register_game_mpr()` first and takes its window and
message box titles from `info().title`. Its own names (`mpr.log`, `mpr.ini`, `--version` text) stay: they belong to the game exe.

### tools: setup

| Where (before) | Kind | Now |
| --- | --- | --- |
| `src/pipeline.h` expected title id and build id | (a) | `src/game.h` `GameProfile`, defined in `src/games/mpr.cpp` (`MPRS_GAME`); the options still default to them and the CLI can still override |
| `src/pipeline.cpp` "v1.0.0" in messages and the display version, `mpr.ini`, `shader_cache.mprshc` | (a) | `game().version`, `game_ini`, `shader_pack` |
| `src/container.cpp` "Setup needs Metroid Prime Remastered", "original v1.0.0" | (a) | `game().title`, `game().version` |
| `gui/main_win32.cpp` window title, "Metroid Prime Remastered found" | (a) | `game().product`, `game().title` |
| `gui/setup.rc`, `setup.manifest` | (c) | not moved (gap) |
| texture cache, program scan (`texcache.cpp`, `programs.cpp`, `pak.cpp`, `txtr.cpp`): Retro pak/TXTR formats | (c) | not moved (gap) |

### tools: release builder

| Where (before) | Kind | Now |
| --- | --- | --- |
| `make_release.py` `mpr.exe`, setup exes, launcher, `mpr.ini.example`, `shader_cache.mprshc`, magic `MPRSHC01`, `mpr-<version>-windows-x64` | (a) | `tools/release/games/mpr.json`; the module reads it at import |
| `templates/README.txt`, `HELP.txt` | (c) | not moved (gap) |

### recomp

The recompiler (`nxrecomp/`) has no game knowledge. `0x7100000000` there is the console's load address for main, a platform fact.
`scripts/build_mpr.cmd` is now a wrapper around a generic `scripts/build_game.cmd <id>`, which builds `games/<id>` (target
`<id>_modules`, repository root passed as `NX_ROOT`).

## Gaps

These are still Metroid-shaped in the base. None of them blocks a second game from building; they limit what it can reuse.

- **Play-mode vocabulary.** `Act` and `Ctx` in `play_types.h` (beams, visors, morph ball, scan), the `Settings` schema (aim, camera,
  morph, ball orbit, wheel beams), the action names in `controls.ini` and the engine's beam cycling (`play_engine.cpp`). A second
  game needs these to become a game-provided action table.
- **Moved code keeps namespace `nxrt::play`.** Done to keep the move mechanical; rename to `nxrt::games::mpr` later.
- **Test tooling vocabulary.** `tt::GameState` fields (morph, beam, visor, missiles), the hash over them (`tt_core.cpp`) and the item
  names in `tt_script.cpp`.
- **Launcher names.** Target `mpr_launcher`, library `mpr_launcher_core`, namespace `mprl`, window class `MprLauncherWindow`.
- **Base tests register the Metroid module.** The service, timestep, tt and play tests run with `NXRT_TEST_GAME=mpr`; the moved
  Metroid tests are in `games/mpr/tests`.
- **Setup tool.** The texture and program stages read Retro's formats; the version resources and the window class name are Metroid's.
- **Release templates** (README, HELP) and the release's licence notice name the game.
- **Docs.** Older stream docs (S*.md) and CONFIG.md still give the old paths of the moved files.
- **Shader program list magic** `MPRPRG01` (`nvn_gpl.cpp`, setup `programs.cpp`) is a file format name shared by the base and the
  setup tool; harmless, but named after the game.

## Adding a game

1. `runtime/games/<id>/` with a CMakeLists that builds `nxrt_game_<id>_data` (and `_core`, and the full library if it has play
   mode), an `Info` and `nxrt_register_game_<id>*()` functions; `launcher.rc` if it ships the launcher.
2. A game exe that calls the register function before `nxrt::init()`.
3. `recomp/games/<id>/` with a `<id>_modules` target; build it with `scripts\build_game.cmd <id>`.
4. `tools/setup/src/games/<id>.cpp` and `tools/release/games/<id>.json`.
