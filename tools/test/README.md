# tools/test: test tooling and Eden reference captures (work area S5)

Host-side half of the test tooling. The runtime half (scripted runs, traces, deterministic mode, record/replay) is `runtime/docs/S5.md`. Python is `tools\.venv\Scripts\python` (numpy and Pillow only); tests are stdlib `unittest`:

    cd tools\test
    ..\.venv\Scripts\python -I -m unittest discover -s tests -v        everything below (no game needed)

## Scripted runs against the game exe

| Tool | What it does |
|---|---|
| `run_suite.py SUITE.toml [--only a,b] [--tag t] [--exe path] [--log NAME] [--jobs N] [--list]` | Runs each `[[run]]` of a suite against the game exe headless and prints one PASS/FAIL table (PASS, FAIL, CRASH, FREEZE, TIMEOUT, ERROR). Exit code 0 only when every run passes. A run can repeat (the final hashes must match), compare its trace with a reference (`ref_trace`), compare screenshots (`ref_shots`) and expect failure (`expect_exit = 1`). The game log name comes from the suite (`log`) or `--log`. Format and exit-code contract: top of the file. |

## Comparators

| Tool | What it does |
|---|---|
| `imgdiff.py A.png B.png [--mask hud\|x0,y0,x1,y1\|mask.png] [--min-ssim 0.9] [--diff-out d.png]` | Perceptual image diff: SSIM on luma, mean absolute error, histogram correlation, fraction of changed pixels, after resizing both to a common grid. Masks (rectangles as fractions, a preset `hud`, or an image) exclude HUD and animated regions. Exit 0 pass, 1 fail, 2 usage. |
| `tracediff.py A.jsonl B.jsonl [--tol pos=0.05,...] [--align first-change:area] [--exact-hash]` | Per-frame trace diff with tolerances (our trace against an Eden trace, or `--exact-hash` for two of our own runs). Reports the first divergence. |
| `regress.py summarize RUN` / `compare BASE CAND` | Run summary (fps median and 10th percentile, p99 frame time, peak memory, crashes, glitch frames) and a no-loss/loss verdict between two runs, thresholds from SuperMarioWonderRecomp `docs/REGRESSION.md`. `find_glitches.py` flags near-black or flat frames among normal ones. |

Trace format (JSON lines, shared by the runtime and the Eden side): `{"f":12,"h":"<16 hex>","world":"<32 hex>","area":"<32 hex>","pos":[x,y,z],"vel":[..],"yaw":..,"hp":..,"igt":..,"state":..,"t":..,"ms":..,"mem":..}`, a header line `{"trace":1,"source":"nxrt"|"eden"}`, and `{"f":..,"label":".."}` lines for `@hash` checkpoints.

## Eden reference harness

Eden v0.2.1 (`toolchain/eden`) runs in portable mode from `work\eden` (a `user\` folder next to the exe; the key files are copied only into `work\eden\user\keys`; `work` is git-ignored). Reference captures are game-derived and live under `work\eden_ref` (never in git).

| Tool | What it does |
|---|---|
| `eden_config.py setup` | Creates the portable folder, installs the keys, writes the harness settings (GDB stub on port 6543, null audio, 1x resolution) into `user\config\sdl2-config.ini`. |
| `eden_run.py --out DIR --every N --seconds S [--timeline t.json]` | Launches `eden-cli` on the NSP with the LunarG screenshot layer, converts the captured frames to PNG, posts timed key presses, optionally samples game state over GDB. |
| `edeninput.py` | Posts WM_KEYDOWN/UP to the Eden window (works in a disconnected session, where SendInput does not). Switch buttons map to Eden's default keyboard profile. |
| `wincap.py` | Window capture (PrintWindow / BitBlt / screen). Only useful in an interactive session: in a disconnected one Eden's window comes out black, which is why frames come from the Vulkan layer. |

Facts the harness is built on (research notes in `runtime/docs/debug/10x.md`): a GDB client connecting pauses the game and it stays paused until a continue; `monitor get info` prints the module list (the main module is the one named for the game's executable); `monitor` replies are hex-encoded text; the screenshot layer is driven by `VK_SCREENSHOT_FRAMES` (a comma list of present numbers) and `VK_SCREENSHOT_DIR`; with the GDB stub switched on Eden WAITS at start for a client (eden_run.py switches it on only for runs that sample state); the main module base changes from run to run (ASLR), `edengdb` asks for it.

Reference captures: `collect_refs.py <timeline json>` copies named frames from exploration sessions into the game's `work\eden_ref`; `contact_sheet.py` makes one overview image of a session. Timeline files: `timelines/*.json`. A new game can take a long time in Eden to reach its first gameplay frames (level loading, shader building), so a session is long.

Game-specific test content (suites, scripts, timelines, region tables, `gameplay.py`, `edengdb.py`, `phys_table.py`, `sweep.py`, the S11/S13 probes) lives in each game: `games/<id>/tools/test`. `eden_run.py`, `eden_config.py` and `collect_refs.py` take the game folder from `NX_GAME_DIR` (default: the current folder) and load `edengdb` from `<game>/tools/test`.
