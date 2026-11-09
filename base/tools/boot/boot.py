r"""Boot loop for one game: rebuild what changed, run the game exe headless with a timeout, print the tail of the log and a summary of every
unknown call (SVC, IPC command, NVN function, HLE stub), the stop reason and the guest call stack (demangled).

  base\tools\.venv\Scripts\python base\tools\boot\boot.py --game <id | game folder> [--wt <name | worktree folder>] [options]

Game: a game id (games\<id> of the main checkout, NX) or a game folder (holds game.json; its checkout is the one holding it). --wt NAME
takes the game and the base from the worktree NX\wt\NAME instead (a worktree is a whole checkout: git worktree add NX\wt\NAME -b <branch>).
The exe, the lifted libraries, the RomFS and the run folder follow base docs\BUILD.md:
  exe    NX\build\<checkout name>\<id>\release\<files.exe of game.json>   (checkout name: main, or the worktree's name: NX\build\x\mpr)
  libs   NX\games\<id>\work\build\<id>_<name> when it exists (a worktree's own lift), else ...\work\build\<id>
  run    NX\games\<id>\work\run (--run NAME: work\run_NAME), log <files.log of game.json>, previous log <log>.prev
  data   game\ and work\ are read from NX\games\<id> of the main checkout (a worktree has none), or from the game folder when it holds them
The test-run rules (scripts\testrun_env.py: headless, 1280x720, NXRT_NVN_POOL_MIRROR=off, NXRT_AUDIO_SINK=paced) and a timeout apply. What one game needs on
top lives in its folder, tools\boot\presets.json: "romfs" and "exefs" (relative to the data folder), "env", "args" (exe arguments),
"seconds", and named "presets" {name: {env, args, seconds}}; values may use {data} {game} {run} {nx} {id}. Env a game needs on every
run (not only in test runs) belongs in its Info::config_defaults instead.

  --seconds N     wall-clock limit for the run (default 40, or the preset's); the exe is also given --max-seconds N-2 so it dumps its state first
  --tail N        last N log lines to print (default 30)
  --libs          also rebuild the lifted game libraries (base recomp\scripts\build_game.cmd, ~2 min, incremental)
  --no-build      run the existing exe
  --wt NAME       use the worktree NX\wt\NAME (or a worktree folder): its game folder, its base scripts, its build folder NX\build\NAME
  --base DIR      base folder whose scripts build (default: the base of the checkout used: main, or the --wt worktree)
  --exe FILE      run this exe instead of the build folder's        --lib-dir DIR  lifted libraries to link (default as above)
  --run NAME      run folder work\run_NAME instead of work\run
  --preset NAME   a named preset of the game's presets.json (repeatable): its env, exe arguments and seconds are added
  --level L       NXRT_LOG_LEVEL for the run (info|debug|trace)
  --link M        [boot] link: rtld (default; the game's own dynamic linker relocates) | host (the runtime's linker)
  --env K=V       extra environment variable (repeatable), e.g. --env NXRT_SVC_UNKNOWN_LOG_LIMIT=1000
  --arg A         extra exe argument (repeatable)
  --grep RE       also print every log line matching RE (case-insensitive)
  --log-only      do not run; summarise the log of the last run
  --dry-run       print the build and run commands, the folders and the environment the run would use, and stop

Exit code: the exit code of the game exe (124 = deadline, 3/4 = crash).

Other tools start a game the same way through this module (sys.path: base\tools\boot):
  import boot
  g = boot.load_game('mpr', exe=DIR_OR_FILE, run=NAME_OR_NONE)   # a Game: .dir .data .id .exe .exe_name .log_name .run .presets
  r = boot.launch(g, env={...}, args=[...], run_dir=..., cwd=..., stdout=FILE, detach=False)   # a Run: .pid .log .wait(timeout) -> exit code
launch() adds the data env of every run of that game (RomFS, save folder, shader cache, BC7 cache, regions file, the presets' env), the
exe's --log and --exefs; the caller adds only what is its own. Presets key "regions" names the guest-address table for scripted runs.
"""
import argparse, json, os, re, subprocess, sys, time, collections

BASE_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(BASE_ROOT, 'scripts'))
import nxpaths
from testrun_env import testrun_env

NX = nxpaths.NX_ROOT        # the main checkout: games\<id>\game and work (the data), build\ and wt (a worktree has none)
CXXFILT = os.path.join(os.environ.get('LLVM_BIN') or os.path.join(os.environ.get('ProgramFiles', r'C:\Program Files'), 'LLVM', 'bin'),
                       'llvm-cxxfilt.exe')
annotate = lambda text: text      # replaced by the game's own address namer (load_addrname)


class Game:
    """One game: its folder (manifest, hooks, presets), its data (game\\, work\\) and the paths BUILD.md derives from them."""

    def __init__(self, arg, a=None):
        a = a or argparse.Namespace()
        base, exe, lib_dir, run, wt = (getattr(a, k, None) for k in ('base', 'exe', 'lib_dir', 'run', 'wt'))
        gid = os.path.basename(os.path.abspath(arg))
        if wt:                                                                # a worktree: a whole checkout under NX\wt
            self.checkout = os.path.abspath(wt if os.path.isdir(wt) else os.path.join(NX, 'wt', wt))
            if not os.path.isdir(os.path.join(self.checkout, 'base')):
                sys.exit('no worktree "%s": give its name (a folder NX/wt/NAME) or its folder (it holds base/ and games/)' % wt)
            self.dir = os.path.join(self.checkout, 'games', gid)
        elif os.path.isfile(os.path.join(arg, 'game.json')):                  # a game folder: the checkout that holds it
            self.dir = os.path.abspath(arg)
            self.checkout = os.path.dirname(os.path.dirname(self.dir))
        else:                                                                 # a game id: the main checkout
            self.checkout = NX
            self.dir = os.path.join(NX, 'games', arg)
        if not os.path.isfile(os.path.join(self.dir, 'game.json')):
            sys.exit('no game "%s" (%s has no game.json): give a game id or a game folder, with --wt <worktree> for a worktree' % (arg, self.dir))
        self.man = json.load(open(os.path.join(self.dir, 'game.json'), encoding='utf-8'))
        self.id = self.man['id']
        self.name = nxpaths.checkout_name(self.checkout, NX)                  # main, or the worktree's folder name
        topic = '' if self.name == 'main' else self.name
        own_data = os.path.isdir(os.path.join(self.dir, 'game')) or os.path.isdir(os.path.join(self.dir, 'work'))
        self.data = self.dir if own_data else os.path.join(NX, 'games', self.id)
        self.base = os.path.abspath(base or os.path.join(self.checkout, 'base'))
        self.build_dir = os.path.join(NX, 'build', self.name, self.id, 'release')
        files = self.man.get('files', {})
        self.exe_name = files.get('exe', self.id + '.exe')
        self.exe = os.path.abspath(exe) if exe else os.path.join(self.build_dir, self.exe_name)
        if os.path.isdir(self.exe):
            self.exe = os.path.join(self.exe, self.exe_name)
        self.exe_dir = os.path.dirname(self.exe)
        self.log_name = files.get('log', self.id + '.log')
        libs = os.path.join(self.data, 'work', 'build', self.id)
        own = os.path.join(self.data, 'work', 'build', self.id + '_' + topic) if topic else ''
        self.libs = os.path.abspath(lib_dir) if lib_dir else (own if own and os.path.isdir(own) else libs)
        self.run = os.path.join(self.data, 'work', 'run_' + run if run else 'run')
        pf = os.path.join(self.dir, 'tools', 'boot', 'presets.json')
        self.presets = json.load(open(pf, encoding='utf-8')) if os.path.isfile(pf) else {}

    def expand(self, s):
        out = (s.replace('{data}', self.data).replace('{game}', self.dir).replace('{run}', self.run)
                .replace('{nx}', NX).replace('{id}', self.id))
        return os.path.normpath(out) if out != s and re.fullmatch(r'\{[a-z]+\}[\/].*', s) else out


def load_addrname(game):
    """The game's own guest-address namer (games\\<id>\\tools\\boot\\addrname.py), or the identity."""
    p = os.path.join(game.dir, 'tools', 'boot', 'addrname.py')
    if not os.path.isfile(p):
        return lambda text: text
    os.environ.setdefault('NX_GAME_DATA', game.data)
    sys.path.insert(0, os.path.dirname(p))
    try:
        import addrname
    except Exception as e:
        print('warning: %s did not import (%s: %s); call stacks are not named' % (p, type(e).__name__, e), file=sys.stderr)
        return lambda text: text

    def namer(text):
        try:
            return addrname.annotate(text)
        except Exception:
            return text
    return namer


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, **kw)


def demangle(text):
    names = set(re.findall(r'\b_Z[A-Za-z0-9_$.]+', text))
    if not names or not os.path.exists(CXXFILT):
        return text
    try:
        out = subprocess.run([CXXFILT] + sorted(names), capture_output=True, text=True, timeout=30).stdout.splitlines()
    except (OSError, subprocess.SubprocessError) as e:
        print('warning: llvm-cxxfilt failed (%s); names stay mangled' % e, file=sys.stderr)
        return text
    if len(out) == len(names):
        for n, d in zip(sorted(names), out):
            text = text.replace(n, d)
    return text


def build_cmds(g, libs):
    """[(label, command line, env additions, cwd)]: the lift (with --libs) and the exe build of base docs\\BUILD.md."""
    cmds = []
    if libs:
        bg = os.path.join(g.base, 'recomp', 'scripts', 'build_game.cmd')
        cmds.append(('game libraries (recomp\\scripts\\build_game.cmd)', '"%s" "%s"' % (bg, g.dir),
                     {'NX_BUILD': g.libs, 'SCCACHE_DIR': os.path.join(g.data, 'work', 'build', 'sccache')}, g.dir))
    ps = os.path.join(g.base, 'runtime', 'scripts', 'build_game_exe.ps1')
    cmds.append(('%s (runtime/scripts/build_game_exe.ps1)' % os.path.basename(g.exe),
                 'pwsh -NoProfile -File "%s" -Game "%s" -LibDir "%s"' % (ps, g.dir, g.libs), {}, g.dir))
    return cmds


def build(g, libs):
    for label, cmd, add, cwd in build_cmds(g, libs):
        print('== ' + label)
        t = time.time()
        r = sh(cmd, cwd=cwd, capture_output=True, text=True, env=dict(os.environ, MSYS2_ARG_CONV_EXCL='*', **add))
        out = r.stdout + r.stderr
        if label.startswith('game libraries'):
            print(out.strip().splitlines()[-1] if out.strip() else '(no output)')
            if r.returncode:
                print('LIBRARY BUILD FAILED'); print(out[-3000:]); sys.exit(10)
        else:
            errs = [l for l in out.splitlines() if ' error' in l or 'error:' in l or 'FAILED' in l or 'undefined symbol' in l]
            if r.returncode:
                print('BUILD FAILED'); print('\n'.join(errs[:30]) or out[-3000:]); sys.exit(11)
            print('built in %.0fs' % (time.time() - t))


def game_env(g, layers=()):
    """The environment every run of this game needs: RomFS, save folder, shader and BC7 caches, the regions table, then the env of `layers`
    (named-preset dicts) over the presets file's own env."""
    env = {
        'NXRT_FS_ROMFS': os.path.normpath(os.path.join(g.data, g.presets.get('romfs', 'game/romfs'))),
        'NXRT_FS_SAVE_DIR': os.path.join(g.run, 'save'),
        'NXRT_NVN_SHADER_CACHE': os.path.join(g.data, 'work', 'shader_cache', 'spirv'),
        'NXRT_NVN_BC7_CACHE': os.path.join(g.data, 'work', 'run', 'bc7_cache'),
    }
    if g.presets.get('regions'):
        env['NXRT_TEST_REGIONS'] = g.expand(g.presets['regions'])
    for p in [g.presets] + list(layers):
        for k, v in p.get('env', {}).items():
            env[k] = g.expand(v)
    return env


def command(g, args=(), run_dir=None):
    """Exe command line: --log, the arguments, then --exefs when the game's presets name one."""
    cmd = [g.exe, '--log', os.path.join(run_dir or g.run, g.log_name)] + list(args)
    if g.presets.get('exefs'):
        cmd += ['--exefs', os.path.normpath(os.path.join(g.data, g.presets['exefs']))]
    return cmd


class Run:
    """A started game process: .pid, .log, .proc; wait() returns the exit code (124 after the deadline kill)."""

    def __init__(self, proc, log):
        self.proc, self.pid, self.log = proc, proc.pid, log

    def wait(self, timeout=None):
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return 124


def launch(g, env=None, args=(), run_dir=None, cwd=None, stdout=None, detach=False):
    """Start the game exe: data env of game_env(), then `env`; cwd defaults to the run folder; stdout (a file object) gets stdout and stderr."""
    run_dir = run_dir or g.run
    os.makedirs(run_dir, exist_ok=True)
    e = dict(os.environ)
    e.update(game_env(g))
    e.update(env or {})
    flags = 0x00000008 | 0x00000200 if detach else 0            # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    p = subprocess.Popen(command(g, args, run_dir), cwd=cwd or run_dir, env=e, stdout=stdout, stderr=subprocess.STDOUT if stdout else None,
                         creationflags=flags)
    return Run(p, os.path.join(run_dir, g.log_name))


def load_game(arg, base=None, exe=None, lib_dir=None, run=None):
    return Game(arg, argparse.Namespace(base=base, exe=exe, lib_dir=lib_dir, run=run))


def run_plan(g, a):
    """(exe arguments, env additions over os.environ, seconds): the test-run rules, the game's presets, then the command line."""
    layers = [g.presets]
    for n in (a.preset or []):
        if n not in g.presets.get('presets', {}):
            sys.exit('no preset "%s" in %s (has: %s)' % (n, os.path.join(g.dir, 'tools', 'boot', 'presets.json'),
                                                         ', '.join(g.presets.get('presets', {})) or 'none'))
        layers.append(g.presets['presets'][n])
    env = testrun_env()
    for k in list(env):
        if k in os.environ:
            env[k] = os.environ[k]
    env.update(game_env(g, layers[1:]))
    for k in ('NXRT_FS_ROMFS', 'NXRT_FS_SAVE_DIR'):
        env[k] = os.environ.get(k, env[k])
    exe_args, seconds = [], None
    for p in layers:
        exe_args += [g.expand(x) for x in p.get('args', [])]
        seconds = p.get('seconds', seconds)
    if a.level: env['NXRT_LOG_LEVEL'] = a.level
    for kv in a.env or []:
        k, v = kv.split('=', 1); env[k] = v
    secs = a.seconds or seconds or 40
    args = ['--max-seconds', str(max(1, secs - 2))] + (['--link', a.link] if a.link else []) + exe_args + (a.arg or [])
    return args, env, secs


def run(g, a, args, env, secs):
    os.makedirs(g.run, exist_ok=True)
    log = os.path.join(g.run, g.log_name)
    if os.path.exists(log):
        os.replace(log, log + '.prev')
    print('== run (%ds limit): %s' % (secs, ' '.join(args)))
    t = time.time()
    r = launch(g, env, args, stdout=open(os.path.join(g.run, 'run.out'), 'w'))
    rc = r.wait(secs)
    if rc == 124:
        print('(killed at the host deadline)')
    print('exit code %d after %.1fs' % (rc, time.time() - t))
    return rc


def dry_run(g, a):
    args, env, secs = run_plan(g, a)
    cmd = command(g, args)
    print('game     %s (%s)  manifest %s' % (g.id, g.name, os.path.join(g.dir, 'game.json')))
    print('base     ' + g.base)
    print('data     ' + g.data)
    print('libs     ' + g.libs)
    print('run dir  ' + g.run)
    print('exe      %s%s' % (g.exe, '' if os.path.exists(g.exe) else '   (not built yet)'))
    if a.no_build:
        print('build    skipped (--no-build)')
    else:
        for label, c, add, cwd in build_cmds(g, a.libs):
            print('build    %s  [cwd %s]%s' % (c, cwd, ''.join('  %s=%s' % kv for kv in add.items())))
    print('run      %s  [cwd %s, %ds]' % (' '.join('"%s"' % c if ' ' in c else c for c in cmd), g.run, secs))
    for k in sorted(env):
        print('env      %s=%s' % (k, env[k]))


def summarise(g, a):
    log = os.path.join(g.run, g.log_name)
    lines = open(log, errors='replace').read().splitlines()
    text = '\n'.join(lines)
    print('\n== summary of %s (%d lines)' % (log, len(lines)))
    stop = [l for l in lines if re.search(r'HOST EXCEPTION|GUEST TRAP|svcBreak reason|svcExitProcess|uncaught C\+\+|max_seconds reached|returned \(x0|FREEZE|freeze', l)]
    print('-- stop:')
    for l in stop[:6]: print('  ' + annotate(l[:300]))
    m = re.search(r'guest call stack \(innermost first\): (.*)', text)
    bt = [l for l in lines if 'guest backtrace' in l or 'guest frame chain' in l]
    if m:
        print('-- guest call stack (innermost first):')
        for f in m.group(1).split()[:14]: print('  ' + annotate(f))
    for l in bt[:3]: print('  ' + annotate(l[:500]))
    unk = collections.OrderedDict()
    for l in lines:
        mm = re.search(r'UNKNOWN SVC (0x[0-9A-Fa-f]+) \((\w+)\) #(\d+) (x0=.*?) caller', l)
        if mm: unk.setdefault((mm.group(1), mm.group(2)), [0, mm.group(4)])[0] = int(mm.group(3))
    print('-- unknown SVCs: %s' % ('none' if not unk else ''))
    for (n, nm), (c, args) in unk.items(): print('  %s %s x%d first %s' % (n, nm, c, args[:150]))
    st = collections.OrderedDict()
    for l in lines:
        mm = re.search(r'STUB SERVICE (\S+) cmd (\d+) \((.*?)\)', l)
        if mm: st.setdefault((mm.group(1), int(mm.group(2))), [0, mm.group(3)])[0] += 1
    sm = [re.search(r'GetServiceHandle\((\S+)\)', l).group(1) for l in lines if 'GetServiceHandle(' in l]
    print('-- services requested: %s' % (', '.join(collections.OrderedDict.fromkeys(sm)) or 'none'))
    print('-- stub service commands: %s' % ('none' if not st else ''))
    for (s, c), (n, d) in st.items(): print('  %s cmd %d (logged x%d) %s' % (s, c, n, d[:120]))
    nvn = collections.OrderedDict()
    for l in lines:
        mm = re.search(r'nvn: STUB (\w+)\(|nvn: UNKNOWN NVN PROC (\w+)\(', l)
        if mm: nvn.setdefault(mm.group(1) or mm.group(2), l[:260])
    print('-- NVN functions hit: %s' % ('none' if not nvn else ''))
    for k, v in list(nvn.items())[:40]: print('  ' + v)
    hle = [l for l in lines if 'shell installed' not in l and re.search(r'logging stub|has no host implementation|UNKNOWN NVN PROC|hle stub called', l)]
    print('-- HLE / import stubs: %s' % ('none' if not hle else ''))
    for l in hle[:20]: print('  ' + l[:260])
    warn = collections.Counter(re.sub(r'0x[0-9a-fA-F]+|\d+', 'N', re.sub(r'^\[[^\]]*\] ', '', l))[:140] for l in lines if re.match(r'\[[^\]]* W ', l))
    print('-- warnings (distinct, with counts): %s' % ('none' if not warn else ''))
    for k, c in warn.most_common(12): print('  x%d %s' % (c, k))
    if a.grep:
        print('-- grep %r:' % a.grep)
        for l in [l for l in lines if re.search(a.grep, l, re.I)][:60]: print('  ' + l[:300])
    print('\n== last %d lines' % a.tail)
    print(demangle('\n'.join(l[:300] for l in lines[-a.tail:])))


def main():
    global annotate
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument('--game')
    ap.add_argument('--base')
    ap.add_argument('--wt')
    ap.add_argument('--exe')
    ap.add_argument('--lib-dir')
    ap.add_argument('--run')
    ap.add_argument('--preset', action='append')
    ap.add_argument('--seconds', type=int)
    ap.add_argument('--tail', type=int, default=30)
    ap.add_argument('--libs', action='store_true')
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--level')
    ap.add_argument('--link')
    ap.add_argument('--env', action='append')
    ap.add_argument('--arg', action='append')
    ap.add_argument('--grep')
    ap.add_argument('--log-only', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('-h', '--help', action='store_true')
    a = ap.parse_args()
    if a.help or not a.game:
        print(__doc__)
        return 0 if a.help else 2
    g = Game(a.game, a)
    if a.dry_run:
        dry_run(g, a)
        return 0
    annotate = load_addrname(g)
    rc = 0
    if not a.log_only:
        if not a.no_build: build(g, a.libs)
        args, env, secs = run_plan(g, a)
        rc = run(g, a, args, env, secs)
    summarise(g, a)
    return rc


if __name__ == '__main__':
    sys.exit(main())
