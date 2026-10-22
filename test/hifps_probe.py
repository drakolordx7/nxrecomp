"""S11 physics and timing probe: the same scripted run at 60, 120, 144 and 240 steps per second, compared in game time.

Every run is deterministic (virtual clock, [test] deterministic=1) with clock_step_ticks = 19.2e6 / rate, so one frame is exactly 1/rate seconds
of game time and a run is as slow as the machine but never wrong. The runtime's timestep module then gives the game dt = 1/rate. A scenario is a
script template in seconds (the generator turns seconds into frames for each rate) plus a check that compares the runs.

  tools\\.venv\\Scripts\\python tools\\test\\hifps_probe.py [--rates 60,120,144,240] [--scenarios menu,walk,jump,turn] [--exe DIR] [--keep]

Scenarios
  menu   title screen, A pressed, front-end movies: screenshots at the same game times, informational only. The front end is movies; loading
         takes real time, so at a higher virtual rate the movie starts at another game time and the shots differ in phase (ssim 0.3 to 0.9) with
         no timing error. The timing check for the front end is hifps_timing.py (movie consumption per game second: 1.000 at 60/120/144/240).
  walk   Frigate start room: walk forward 3 s, stop.   steady speed, distance in 3 s, time to 95 % speed, slide distance after release
  jump   Frigate start room: standing jump (B held 0.4 s).   peak height, airtime
  turn   mouse turn of 1000 counts spread over 0.2 s.     total yaw (must be counts * sensitivity at every rate)
  Needs gameplay (S4e): the state probes read the player's position, velocity, yaw and movement state each frame (docs/S5.md).
  Not scripted yet because the probes do not read them: morph-ball roll and friction (needs the ball item and its velocity), projectile range
  (needs a projectile probe), the HUD memo timer (needs the HUD state); docs/S11.md lists what each needs.

Output: work/hifps_probe/<scenario>_<rate>/ (script, mpr.log, trace.jsonl, shots) and a table. Exit code 0 all checks pass, 1 a check fails,
3 gameplay was not reachable for a gameplay scenario (the run did not get past the warp), 2 usage.
"""
import argparse, json, math, os, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
OUT = os.path.join(ROOT, 'work', 'hifps_probe')
TICK_HZ = 19200000


def frames(sec, rate):
    return max(1, int(round(sec * rate)))


# ---- script templates (seconds -> frames) ------------------------------------------------------------------------------------------------
def script_menu(rate):
    F = lambda s: frames(s, rate)
    return '\n'.join([
        '# S11 menu timing: the same game times at every rate',
        '@timeout 0',
        '@frame %d' % F(22.0), '@shot t0',
        'press A for %d' % F(0.1),
        '@frame %d' % F(22.4), '@shot t1',
        '@frame %d' % F(22.8), '@shot t2',
        '@frame %d' % F(23.6), '@shot t3',
        '@frame %d' % F(26.0), '@shot t4',
        '@pass', ''])


def gameplay_head(rate):
    F = lambda s: frames(s, rate)
    return ['@timeout 0', '@warp frigate frigate_start', 'wait %d' % F(3.0), '@assert health > 0']


def script_walk(rate):
    F = lambda s: frames(s, rate)
    return '\n'.join(['# S11 walk probe'] + gameplay_head(rate) + [
        '@hash walk_start', 'stick left 0 1.0', 'wait %d' % F(3.0), 'stick left 0 0.0', 'wait %d' % F(1.5), '@pass', ''])


def script_jump(rate):
    F = lambda s: frames(s, rate)
    return '\n'.join(['# S11 jump probe'] + gameplay_head(rate) + [
        '@hash jump_start', 'press B for %d' % F(0.4), 'wait %d' % F(2.0), '@pass', ''])


def script_turn(rate):
    F = lambda s: frames(s, rate)
    n = F(0.2)
    return '\n'.join(['# S11 turn probe: 1000 counts over 0.2 s'] + gameplay_head(rate) + [
        'mouse %.6f 0 for %d' % (1000.0 / n, n), 'wait %d' % F(0.5), '@pass', ''])


SCENARIOS = {'menu': script_menu, 'walk': script_walk, 'jump': script_jump, 'turn': script_turn}
GAMEPLAY = {'walk', 'jump', 'turn'}


# ---- run ----------------------------------------------------------------------------------------------------------------------------------
def run_one(scenario, rate, exe_dir, max_seconds, regions):
    d = os.path.join(OUT, '%s_%d' % (scenario, rate))
    os.makedirs(d, exist_ok=True)
    spath = os.path.join(d, 'script.nxs')
    with open(spath, 'w') as f:
        f.write(SCENARIOS[scenario](rate))
    env = dict(os.environ)
    env.update(NXRT_TEST_SCRIPT=spath, NXRT_TEST_OUT=os.path.join(d, 'out'), NXRT_TEST_DETERMINISTIC='1', NXRT_TEST_CLOCK_STEP_TICKS=str(int(round(TICK_HZ / rate))),
               NXRT_TEST_REGIONS=regions, NXRT_BOOT_GFX='headless', NXRT_FS_ROMFS=os.path.join(ROOT, 'game', 'romfs'), NXRT_FS_SAVE_DIR=os.path.join(ROOT, 'work', 'run', 'save'),
               NXRT_TEST_WALL_TIMEOUT_S=str(max_seconds), NXRT_TIME_MODE='real')
    cmd = [os.path.join(exe_dir, 'mpr.exe'), '--log', os.path.join(d, 'mpr.log'), '--max-seconds', str(max_seconds)]
    t = time.time()
    try:
        p = subprocess.run(cmd, cwd=os.path.join(ROOT, 'work', 'run'), env=env, capture_output=True, text=True, timeout=max_seconds + 60)
        code = p.returncode
    except subprocess.TimeoutExpired:
        code = 124
    return d, code, time.time() - t


def load_trace(d):
    path = os.path.join(d, 'out', 'trace.jsonl')
    rows = []
    if os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if line.endswith('}'):
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    pass
    return rows


# ---- metrics --------------------------------------------------------------------------------------------------------------------------------
def walk_metrics(rows, rate):
    """rows after the 'walk_start' frame: speed per frame from positions (units / s of game time)."""
    pts = [(r['f'], r['pos']) for r in rows if 'pos' in r]
    if len(pts) < 10:
        return None
    sp = []
    for (f0, p0), (f1, p1) in zip(pts, pts[1:]):
        sp.append((f1, math.hypot(p1[0] - p0[0], p1[1] - p0[1]) * rate / max(1, f1 - f0)))
    start = next((f for f, s in sp if s > 0.05), None)
    if start is None:
        return None
    walk_frames = int(3.0 * rate)
    seg = [s for f, s in sp if start <= f < start + walk_frames]
    steady = sum(seg[-int(1.5 * rate):]) / max(1, len(seg[-int(1.5 * rate):]))
    t95 = next((i / rate for i, s in enumerate(seg) if s >= 0.95 * steady), None)
    dist = sum(s / rate for s in seg)
    tail = [s for f, s in sp if f >= start + walk_frames]
    slide = sum(s / rate for s in tail)
    return dict(steady=steady, dist=dist, t95=t95, slide=slide)


def jump_metrics(rows, rate):
    pts = [(r['f'], r['pos'][2]) for r in rows if 'pos' in r]
    if len(pts) < 10:
        return None
    z0 = pts[0][1]
    peak = max(z for _, z in pts) - z0
    air = [f for f, z in pts if z > z0 + 0.02]
    return dict(peak=peak, air=(len(air) / rate) if air else 0.0)


def turn_metrics(rows, rate):
    ys = [r['yaw'] for r in rows if 'yaw' in r]
    if len(ys) < 10:
        return None
    d = ys[-1] - ys[0]
    while d > math.pi:
        d -= 2 * math.pi
    while d < -math.pi:
        d += 2 * math.pi
    return dict(yaw=d)


METRICS = {'walk': (walk_metrics, {'steady': 0.01, 'dist': 0.02, 't95': 0.06, 'slide': 0.06}),
           'jump': (jump_metrics, {'peak': 0.02, 'air': 0.03}),
           'turn': (turn_metrics, {'yaw': 0.01})}


def check_menu(dirs, a):
    import imgdiff
    base = 60 if 60 in dirs else min(dirs)
    ok = True
    print('%-6s %s' % ('shot', ' '.join('%8s' % ('%d Hz' % r) for r in sorted(dirs))))
    for shot in ('t0', 't1', 't2', 't3', 't4'):
        cells = []
        for r in sorted(dirs):
            pa = os.path.join(dirs[base], 'out', 'shots', shot + '.png')
            pb = os.path.join(dirs[r], 'out', 'shots', shot + '.png')
            if not (os.path.exists(pa) and os.path.exists(pb)):
                cells.append('missing')
                continue
            s = 1.0 if r == base else imgdiff.compare(pa, pb).ssim
            cells.append('%8.4f' % s)
        print('%-6s %s' % (shot, ' '.join('%8s' % c for c in cells)))
    print('(informational: not a pass/fail check, see the scenario text)')
    return True


def check_gameplay(scenario, dirs, a):
    fn, tol = METRICS[scenario]
    res = {}
    for r, d in dirs.items():
        rows = load_trace(d)
        res[r] = fn(rows, r) if rows else None
    if any(v is None for v in res.values()):
        print('%s: no player samples at rates %s (gameplay not reachable: the run did not get past the warp)' % (scenario, [r for r, v in res.items() if v is None]))
        return None
    base = 60 if 60 in res else min(res)
    ok = True
    keys = list(tol)
    print('%-6s %s' % ('Hz', ' '.join('%10s' % k for k in keys)))
    for r in sorted(res):
        cells = []
        for k in keys:
            v, b = res[r][k], res[base][k]
            dev = abs(v - b) / abs(b) if b else (0 if v == b else 1)
            bad = dev > tol[k]
            ok = ok and not bad
            cells.append('%8.4f%s' % (v, '!' if bad else ' '))
        print('%-6d %s' % (r, ' '.join('%10s' % c for c in cells)))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rates', default='60,120,144,240')
    ap.add_argument('--scenarios', default='menu,walk,jump,turn')
    ap.add_argument('--exe', default=os.path.join(ROOT, 'game-build', 'build', 'hifps'))
    ap.add_argument('--max-seconds', type=int, default=300)
    ap.add_argument('--ssim', type=float, default=0.97)
    ap.add_argument('--regions', default=os.path.join(HERE, 'regions.tsv'))
    ap.add_argument('--no-run', action='store_true', help='only analyse the runs already in work/hifps_probe')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    rates = [int(x) for x in a.rates.split(',')]
    status = 0
    for sc in a.scenarios.split(','):
        if sc not in SCENARIOS:
            print('unknown scenario', sc)
            return 2
        print('== %s' % sc)
        dirs = {}
        for r in rates:
            if a.no_run:
                d = os.path.join(OUT, '%s_%d' % (sc, r))
            else:
                d, code, took = run_one(sc, r, a.exe, a.max_seconds, a.regions)
                print('   %d Hz: exit %d in %.0f s' % (r, code, took))
            dirs[r] = d
        ok = check_menu(dirs, a) if sc == 'menu' else check_gameplay(sc, dirs, a)
        if ok is None:
            status = max(status, 3)
        elif not ok:
            print('%s: FAIL' % sc)
            status = max(status, 1)
        else:
            print('%s: PASS' % sc)
    return status


if __name__ == '__main__':
    sys.exit(main())
