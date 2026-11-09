"""The NX folders for a base Python script, by the same rule as scripts/env.cmd and cmake/nx_game.cmake.

    NX_BASE   the base checkout: env NX_BASE, else the folder holding this file's folder
    NX_ROOT   the folder holding games/ and build/: env NX_ROOT, else the nearest parent of the base checkout with a games/ folder
              (None outside one)
    python_exe()   env NX_PYTHON, else base/tools/.venv, else NX_ROOT/base/tools/.venv, else the running interpreter
"""
import os
import sys

NX_BASE = os.path.abspath(os.environ.get('NX_BASE') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def find_root(start=NX_BASE):
    env = os.environ.get('NX_ROOT')
    if env:
        return os.path.abspath(env)
    d = os.path.abspath(start)
    while True:
        up = os.path.dirname(d)
        if up == d:
            return None
        d = up
        if os.path.isdir(os.path.join(d, 'games')):
            return d


NX_ROOT = find_root()


def python_exe():
    env = os.environ.get('NX_PYTHON')
    if env:
        return env
    for base in (NX_BASE, os.path.join(NX_ROOT, 'base') if NX_ROOT else None):
        if base:
            p = os.path.join(base, 'tools', '.venv', 'Scripts', 'python.exe')
            if os.path.exists(p):
                return p
    return sys.executable


def game_dir(game):
    """games/<id> below NX_ROOT, or `game` itself when it is a folder with a game.json."""
    if os.path.isfile(os.path.join(game, 'game.json')):
        return os.path.abspath(game)
    if not NX_ROOT:
        raise SystemExit('no NX folder around %s (set NX_ROOT)' % NX_BASE)
    return os.path.join(NX_ROOT, 'games', game)
