"""The NX folders for a base Python script, by the same rule as scripts/env.cmd and cmake/nx_game.cmake.

    NX_BASE      the base folder: env NX_BASE, else the folder holding this file's folder (<checkout>/base)
    NX_CHECKOUT  the checkout holding it: the main checkout (NX_ROOT) or a worktree (NX_ROOT/wt/<name>); the parent of NX_BASE
    NX_ROOT      the main checkout, the folder with the local-only data (games/<id>/game, work), build/, refs/ and wt/: env NX_ROOT, else
                 the parent of wt/ when the checkout is a worktree (its parent folder is named wt), else the checkout itself
    CHECKOUT_NAME  'main' for the main checkout, else the worktree's folder name: the folder under NX_ROOT/build holding its output
    python_exe()   env NX_PYTHON, else base/tools/.venv, else NX_ROOT/base/tools/.venv, else the running interpreter
    IS_WINDOWS, PLATFORM ('windows' | 'linux'), EXE_SUFFIX ('.exe' | ''), exe_name(name) (a manifest file name in this OS's spelling: 'mpr.exe' is
                 'mpr' on Linux), exe_stem(name), venv_python(base), build_dir(name, platform) (NX_ROOT/build/<name>, Linux: .../linux),
                 to_wsl(path) / from_wsl(path) (D:/x <-> /mnt/d/x, for a Windows tool that drives the WSL Linux box)
"""
import os
import sys

IS_WINDOWS = os.name == 'nt'
PLATFORM = 'windows' if IS_WINDOWS else 'linux'
EXE_SUFFIX = '.exe' if IS_WINDOWS else ''

NX_BASE = os.path.abspath(os.environ.get('NX_BASE') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
NX_CHECKOUT = os.path.dirname(NX_BASE)


def find_root(checkout=NX_CHECKOUT):
    env = os.environ.get('NX_ROOT')
    if env:
        return os.path.abspath(env)
    up = os.path.dirname(os.path.abspath(checkout))
    return os.path.dirname(up) if os.path.basename(up).lower() == 'wt' else os.path.abspath(checkout)


NX_ROOT = find_root()


def checkout_name(checkout=NX_CHECKOUT, root=NX_ROOT):
    """'main' for the main checkout, else its folder name (a worktree NX/wt/x builds into NX/build/x)."""
    checkout = os.path.abspath(checkout)
    return 'main' if os.path.normcase(checkout) == os.path.normcase(os.path.abspath(root)) else os.path.basename(checkout)


CHECKOUT_NAME = checkout_name()


def exe_stem(name):
    """'mpr.exe' -> 'mpr' (the manifest's `files` names are Windows spellings; the stem is the OS-neutral part)."""
    return name[:-4] if name.lower().endswith('.exe') else name


def exe_name(name, platform=None):
    """A program name from game.json `files` in the spelling of `platform` (default: this OS): '.exe' on Windows, none on Linux."""
    return exe_stem(name) + ('.exe' if (platform or PLATFORM) == 'windows' else '')


def venv_python(base, platform=None):
    """The interpreter of <base>/tools/.venv: Scripts/python.exe on Windows, bin/python on Linux."""
    rel = ('Scripts', 'python.exe') if (platform or PLATFORM) == 'windows' else ('bin', 'python')
    return os.path.join(base, 'tools', '.venv', *rel)


def build_dir(name=None, platform=None):
    """NX_ROOT/build/<name> (name: main | the worktree's), with Linux builds below it in linux/ (runtime, <id>, ...)."""
    d = os.path.join(NX_ROOT, 'build', name or CHECKOUT_NAME)
    return os.path.join(d, 'linux') if (platform or PLATFORM) == 'linux' else d


def to_wsl(path):
    """C:/dev/x -> /mnt/d/AI/x (the WSL view of a Windows path); other paths come back with forward slashes."""
    p = os.path.abspath(path).replace(os.sep, '/')
    if len(p) > 2 and p[1] == ':':
        return '/mnt/%s%s' % (p[0].lower(), p[2:])
    return p


def from_wsl(path):
    """/mnt/d/AI/x -> C:/dev/x; other paths come back unchanged."""
    if path.startswith('/mnt/') and len(path) > 6 and path[6:7] == '/':
        return '%s:%s' % (path[5].upper(), path[6:])
    return path


def python_exe():
    env = os.environ.get('NX_PYTHON')
    if env:
        return env
    for base in (NX_BASE, os.path.join(NX_ROOT, 'base')):
        p = venv_python(base)
        if os.path.exists(p):
            return p
    return sys.executable


def game_dir(game):
    """`game` itself when it is a folder with a game.json, else games/<id> of the main checkout (NX_ROOT)."""
    if os.path.isfile(os.path.join(game, 'game.json')):
        return os.path.abspath(game)
    return os.path.join(NX_ROOT, 'games', game)
