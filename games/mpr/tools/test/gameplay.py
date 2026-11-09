"""Gameplay-run helpers for scripted runs on the real game (S13).

  import gameplay as gp
  lines = gp.prologue() + gp.warp('tallon', 'tallon_canyon_cavern') + ['stick left 0 1.0', '@run 3', '@pass']
  res = gp.run(lines, 'work/verify/walk60', rate=60)      # rate: deterministic mode with one frame = 1/rate s of game time; None = real time
  rows = gp.rows('work/verify/walk60')                    # trace rows (dicts) that carry the player

Facts the scripts rely on (all read off the running game, see runtime/docs/S13.md):
  * The world is Y up: forward is +Z at yaw 0 and left is +X, walking speed 12.8 units/s. yaw in the trace is the heading about Y (+ = turn left).
  * The game clock (igt) stands still while an area loads, so waits are written in game seconds with `@run`, not frames.
  * Front-end presses are made with `@mash`, which presses A every few seconds until the new game's first area is current.
  * `@warp world area` loads any area with the player in control after about 4 s of game time; a new game start (Frigate) comes first.
"""
import json, os, subprocess, sys, time

GAME_DIR = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))      # games/mpr, or a worktree of it


def _nx_boot():
    d = os.environ.get('NX_ROOT') or GAME_DIR
    while d and not os.path.isdir(os.path.join(d, 'games')):
        p = os.path.dirname(d)
        d = None if p == d else p
    return os.path.join(d, 'base', 'tools', 'boot') if d else sys.exit('gameplay: NX folder not found (set NX_ROOT)')


sys.path.insert(0, _nx_boot())
import boot  # noqa: E402

G = boot.load_game(GAME_DIR, exe=os.environ.get('NXRT_VERIFY_EXE'))        # exe: NXRT_VERIFY_EXE, else NX/build/<checkout>/release
ROOT = G.data.replace(os.sep, '/')                                          # the game's data folder (game/, work/)
EXE_DIR = G.exe_dir
RUN_DIR = ROOT + '/work/run_verify'
GAME_ID = G.id
EXE_NAME = G.exe_name
LOG_NAME = G.log_name
TICK_HZ = 19200000


def prologue(first_area='frigate_exterior_docking_hangar', timeout_ms=240000):
    """Title -> New Game -> the Frigate's first area (cutscene running, player not yet controllable)."""
    return ['@timeout 0', '@sleep 8000', '@mash A 5000 area %s timeout=%d' % (first_area, timeout_ms), '@run 2']


def warp(world, area, settle=4.0):
    """Warp and wait until the player is in control (game seconds)."""
    return ['@warp %s %s' % (world, area), '@run %g' % settle]


def run(lines, out, rate=None, env=None, seconds=600, gfx='headless', exe_dir=None, watchdog=60):
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    cwd = os.path.join(out, 'cwd')            # own working directory per run: save folder, controls.ini, logs
    os.makedirs(cwd, exist_ok=True)
    script = os.path.join(out, 'script.nxs')
    open(script, 'w').write('\n'.join(lines) + '\n')
    e = dict(NXRT_TEST_SCRIPT=script, NXRT_TEST_OUT=os.path.join(out, 'out'), NXRT_BOOT_GFX=gfx, NXRT_FS_SAVE_DIR=os.path.join(cwd, 'save'),
             NXRT_TEST_WALL_TIMEOUT_S=str(seconds), NXRT_TIME_MODE='real', NXRT_DIAG_WATCHDOG_SECONDS=str(watchdog))
    if gfx == 'headless':
        e['NXRT_GFX_HEADLESS'] = '1'
    if rate:
        e['NXRT_TEST_DETERMINISTIC'] = '1'
        e['NXRT_TEST_CLOCK_STEP_TICKS'] = str(int(round(TICK_HZ / rate)))
    e.update(env or {})
    g = boot.load_game(GAME_DIR, exe=exe_dir) if exe_dir else G
    t = time.time()
    r = boot.launch(g, e, ['--max-seconds', str(seconds)], run_dir=out, cwd=cwd, stdout=subprocess.DEVNULL)
    rc = r.wait(seconds + 60)
    res = dict(exit=rc, wall=time.time() - t)
    rp = os.path.join(out, 'out', 'result.json')
    if os.path.exists(rp):
        res.update(json.load(open(rp)))
    return res


def rows(out, area_prefix='', player_only=True):
    r = []
    for line in open(os.path.join(out, 'out', 'trace.jsonl')):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if 'f' not in d or (player_only and 'pos' not in d) or not d.get('area', '').startswith(area_prefix):
            continue
        r.append(d)
    return r
