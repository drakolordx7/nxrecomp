# tools/test (game side)

Game-specific half of the test tooling; the generic runners (`run_suite.py`, `imgdiff.py`, `tracediff.py`, the Eden launcher) are in the base repo at `base/tools/test`. Run them from the game folder, for example `python ..\..\base\tools\test\run_suite.py tools\test\suites\boot.toml`. `gameplay.py` names the game exe and log after the game folder (`NXRT_GAME_EXE`, `NXRT_GAME_LOG` override).

| Tool | What it does |
|---|---|
| `suites/boot.toml`, `scripts/*.nxs` | What runs today (bring-up determinism) and the first gameplay script (warp to the Frigate, walk). The DSL is documented in `runtime/include/nxrt/tt_script.h`. |
| `regions.tsv`, `gen_regions.py` | World and area names for `@warp` / `@area`, generated from the practice mod's `RegionEntries.h` (284 rows). |
| `phys_table.py --exe DIR [--rates 60,120,144,240] [--scenarios walk,half,jump,rjump,ball] [--tag t]` | Physics table (S22): walk and ball speed, jump height and air time at each step rate, from the player's position in the trace, one game process per rate with `@rate`. Prints the deviation from the first rate. Tests: `tests/test_phys_table.py`. |
| `keys_check.py [--exe DIR]` | Presses the keyboard beam and visor keys in the game and prints the beam and visor numbers the game reports; exit 0 when they are the expected ones. |
| `edengdb.py info\|state\|trace` | GDB remote client for Eden's stub. `state` reads the same fields as the runtime's probes (same offsets), `trace` writes a per-frame JSONL by breaking on `CGameArchitectureSupport::Update`. |

## Eden reference captures (Metroid Prime Remastered)

`collect_refs.py timelines/refs_session1.json` copies named frames from the exploration sessions into `work\eden_ref` (boot logo, title,
main menu, file select, difficulty, loading, the Frigate intro cinematic, an attract-mode gameplay clip, plus a GDB state read
`eden_state_frigate_hangar.json` and a per-frame GDB trace). In Eden `monitor get info` names the main module `GameFramework.nss`. A new
game takes 12 to 35 minutes of wall time to reach the Frigate in Eden on this machine, so a session is long; first gameplay frames were not
reached (see the S5 report).
