#!/usr/bin/env python3
r"""The Linux build driver: the base runtime and its tests, the lifted game libraries, the game exe and the setup tool of one stream, with the
commands of docs/BUILD.md (Linux section). It is the Linux counterpart of wt.ps1; wt.py is the entry point of both systems.

    python3 base/scripts/nx_build.py <name> [--game ID] [--libs] [--libs-at DIR] [--test RE] [--no-exe] [--setup] [--release --version V]
                                     [--clean] [--jobs N] [--ispc FILE] [--deps DIR] [--a2b DIR] [--dry-run]
    python  base/scripts/nx_build.py --wsl ...      from Windows: the same command inside the WSL distro (env NX_WSL_DISTRO, default nx-linux)

  <name>      the stream: the worktree NX/wt/<name>; "main" = the main checkout. Game data (games/<id>/game, work) is read from the main checkout.
  --game ID   also build the game exe (and with --test the game's runtime module and its tests)
  --libs      first lift + build the game's libraries into games/<id>/work/build/linux/<id>[_<name>] (a stream's own lift; main: <id>)
  --libs-at   use (and with --libs fill) that library folder
  --test RE   build the runtime with its tests (plus the game's with --game) and run the ctest names matching RE ("." = all)
  --no-exe    with --game: no exe build        --setup   build the setup cli (+ its tests with --test) into NX/build/setup-<id>-linux
  --release   the package flavour: NXRT_RELEASE=ON and the FFmpeg .so files bundled next to the exe (NXRT_FFMPEG_BUNDLED), with --version
Output: NX/build/<name>/linux/runtime (runtime + tests), NX/build/<name>/linux/<id> (game exe; <id>_relpkg with --release). Without --game
and without --test it only builds the runtime. Exit 0 when every step passed, 1 otherwise (the failed steps are printed last).
ispc (the ASTC -> BC7 encoder's compiler) must be on PATH, or in NX/toolchain/ispc/bin, or given with --ispc / env NX_ISPC.
"""
import argparse
import glob
import json
import os
import shlex
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nxpaths  # noqa: E402

NX = nxpaths.NX_ROOT


def run_in_wsl(argv):
    """Windows: run this script inside the WSL distro (paths of the arguments that name Windows folders are converted)."""
    distro = os.environ.get('NX_WSL_DISTRO', 'nx-linux')
    conv = []
    for a in argv:
        conv.append(nxpaths.to_wsl(a) if len(a) > 2 and a[1] == ':' and a[2] in '\\/' else a)
    script = nxpaths.to_wsl(os.path.abspath(__file__))
    cmd = ['wsl.exe', '-d', distro, '-u', os.environ.get('NX_WSL_USER', 'root'), '--', 'python3', script] + conv
    return subprocess.call(cmd)


def find_ispc(explicit):
    for c in (explicit, os.environ.get('NX_ISPC')):
        if c and os.path.isfile(c):
            return c
    p = shutil.which('ispc')
    if p:
        return p
    pats = [os.path.join(NX, 'toolchain', 'ispc', '**', 'bin', 'ispc'),
            os.path.join(NX, 'base', 'tools', 'textures', 'third_party', 'ispc', '*linux*', 'bin', 'ispc')]
    for pat in pats:
        hits = sorted(glob.glob(pat, recursive=True))
        if hits:
            return hits[-1]
    return None


class Driver:
    def __init__(self, a):
        self.a = a
        self.name = a.name
        self.co = NX if a.name == 'main' else os.path.join(NX, 'wt', a.name)
        if not os.path.isdir(os.path.join(self.co, 'base')) and not a.dry_run:
            sys.exit('no worktree %s (create it: git -C %s worktree add wt/%s -b <branch>)' % (self.co, NX, a.name))
        self.base = os.path.join(self.co, 'base')
        self.bd = os.path.join(NX, 'build', self.name, 'linux')
        self.failed = []
        self.env = dict(os.environ)
        self.jobs = str(a.jobs or os.cpu_count() or 4)
        self.game = a.game
        if self.game:
            self.gdir = os.path.join(self.co, 'games', self.game)
            self.data = os.path.join(NX, 'games', self.game)
            if not os.path.isfile(os.path.join(self.gdir, 'game.json')) and not a.dry_run:
                sys.exit('no game manifest in %s' % self.gdir)
            own = os.path.join(self.data, 'work', 'build', 'linux', '%s_%s' % (self.game, self.name))
            main = os.path.join(self.data, 'work', 'build', 'linux', self.game)
            if a.libs_at:
                self.libs = os.path.abspath(a.libs_at)
            elif a.libs or (self.name != 'main' and os.path.isdir(own)):
                self.libs = main if self.name == 'main' else own
            else:
                self.libs = main
        elif a.libs or a.no_exe:
            sys.exit('--libs and --no-exe need --game <id>')
        ispc = find_ispc(a.ispc)
        self.ispc = ispc
        if ispc:
            self.env['PATH'] = os.path.dirname(ispc) + os.pathsep + self.env.get('PATH', '')
        self.deps = a.deps or os.environ.get('NXRT_DEPS_DIR')

    # -- helpers
    def step(self, label, argv, cwd=None, env=None):
        print('== ' + label, flush=True)
        print('> ' + ' '.join(shlex.quote(x) for x in argv) + ('   [cwd %s]' % cwd if cwd else ''), flush=True)
        if self.a.dry_run:
            return True
        e = dict(self.env)
        e.update(env or {})
        return subprocess.call(argv, cwd=cwd, env=e) == 0

    def cmake_common(self, extra=()):
        c = ['-DNX_ROOT=' + NX]
        if self.deps:
            c.append('-DNXRT_DEPS_DIR=' + self.deps)
        if self.ispc:
            c.append('-DA2B_ISPC=' + self.ispc)
        if self.a.a2b:
            c.append('-DNXRT_A2B_DIR=' + self.a.a2b)
        return c + list(extra)

    def configure(self, label, src, build, extra, clean=False):
        if clean and os.path.isdir(build) and not self.a.dry_run:
            shutil.rmtree(build)
        return self.step(label + ' (configure)', ['cmake', '--preset', 'linux-release', '-B', build] + self.cmake_common(extra), cwd=src)

    def build(self, label, build, targets=(), keep_going=False):
        argv = ['cmake', '--build', build, '-j', self.jobs]
        for t in targets:
            argv += ['--target', t]
        if keep_going:
            argv += ['--', '-k', '0']
        return self.step(label + ' (build)', argv)

    # -- steps
    def libs_step(self):
        gen = self.libs + '_gen'
        src = os.path.join(self.base, 'recomp', 'game')
        extra = ['-DNX_GAME_DIR=' + self.gdir, '-DNX_GAME_EXEFS=' + os.path.join(self.data, 'game', 'exefs'), '-DNX_GAME_GEN=' + gen]
        self.env.setdefault('SCCACHE_DIR', os.path.join(self.data, 'work', 'build', 'sccache', 'linux'))
        ok = self.configure('lift + libraries', src, self.libs, extra, self.a.clean) and self.build('lift + libraries', self.libs, [self.game + '_modules'])
        if not ok:
            self.failed.append('library build')
        return ok

    def runtime_step(self):
        a = self.a
        rt = os.path.join(self.bd, 'runtime')
        extra = []
        if a.test and self.game:
            extra.append('-DNXRT_GAME_DIRS=' + self.gdir)
        ok = self.configure('base runtime', os.path.join(self.base, 'runtime'), rt, extra, a.clean) and self.build('base runtime', rt, keep_going=True)
        if ok and a.test:
            ok = self.step('ctest', ['ctest', '--test-dir', rt, '--output-on-failure', '-R', a.test])
        if not ok:
            self.failed.append('runtime build/tests')

    def exe_step(self):
        a = self.a
        out = os.path.join(self.bd, self.game + ('_relpkg' if a.release else ''))
        extra = ['-DNX_GAME_DIR=' + self.gdir, '-DGAME_LIB_DIR=' + self.libs, '-DGAME_VERSION=' + a.version]
        if a.release:
            extra += ['-DNXRT_RELEASE=ON', '-DNXRT_FFMPEG_BUNDLED=ON']
        src = os.path.join(self.base, 'runtime', 'cmake', 'game_project')
        ok = self.configure('%s exe' % self.game, src, out, extra, a.clean) and self.build('%s exe' % self.game, out, [self.game])
        print('exe: ' + out)
        if not ok:
            self.failed.append('game exe build')

    def setup_step(self):
        a = self.a
        out = os.path.join(NX, 'build', 'setup-%s-linux' % self.game)
        src = os.path.join(self.base, 'tools', 'setup')
        extra = ['-DNX_GAME_DIR=' + self.gdir, '-DNXS_VERSION=' + a.version]
        ok = self.configure('setup tool', src, out, extra, a.clean) and self.build('setup tool', out, keep_going=True)
        if ok and a.test:
            ok = self.step('setup ctest', ['ctest', '--test-dir', out, '--output-on-failure'])
        print('setup: ' + os.path.join(out, 'bin'))
        if not ok:
            self.failed.append('setup tool')

    def go(self):
        a = self.a
        print('stream %s   checkout %s   build %s' % (self.name, self.co, self.bd))
        if self.game:
            print('game %s   libs %s   data %s' % (self.gdir, self.libs, self.data))
        print('ispc: %s' % (self.ispc or 'not found (put ispc on PATH, in NX/toolchain/ispc/bin, or pass --ispc)'))
        if self.game and a.libs:
            if not self.libs_step():
                return 1
        if a.test or not self.game:
            self.runtime_step()
        if self.game and not a.no_exe:
            self.exe_step()
        if a.setup:
            if not self.game:
                sys.exit('--setup needs --game <id>')
            self.setup_step()
        if self.failed:
            print('FAILED: ' + ', '.join(self.failed))
            return 1
        return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if '--wsl' in argv:
        argv.remove('--wsl')
        if nxpaths.IS_WINDOWS:
            return run_in_wsl(argv)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('name')
    ap.add_argument('--game')
    ap.add_argument('--libs', action='store_true')
    ap.add_argument('--libs-at')
    ap.add_argument('--test', default='')
    ap.add_argument('--no-exe', action='store_true')
    ap.add_argument('--setup', action='store_true')
    ap.add_argument('--release', action='store_true')
    ap.add_argument('--version', default='dev')
    ap.add_argument('--clean', action='store_true')
    ap.add_argument('--jobs', type=int)
    ap.add_argument('--ispc')
    ap.add_argument('--deps', help='archive cache of the FetchContent downloads (env NXRT_DEPS_DIR; default ~/.cache/nxrt/deps)')
    ap.add_argument('--a2b', help='a built tools/textures folder (NXRT_A2B_DIR)')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args(argv)
    if nxpaths.IS_WINDOWS:
        sys.exit('nx_build.py builds for Linux: run it in WSL, or add --wsl (Windows builds: pwsh wt.ps1)')
    return Driver(a).go()


if __name__ == '__main__':
    sys.exit(main())
