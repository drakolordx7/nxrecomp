"""The NX folders for a base Python script, by the same rule as scripts/env.cmd and cmake/nx_game.cmake.

    NX_BASE      the base folder: env NX_BASE, else the folder holding this file's folder (<checkout>/base)
    NX_CHECKOUT  the checkout holding it: the main checkout (NX_ROOT) or a worktree (NX_ROOT/wt/<name>); the parent of NX_BASE
    NX_ROOT      the main checkout, the folder with the local-only data (games/<id>/game, work), build/, refs/ and wt/: env NX_ROOT, else
                 the parent of wt/ when the checkout is a worktree (its parent folder is named wt), else the checkout itself
    CHECKOUT_NAME  'main' for the main checkout, else the worktree's folder name: the folder under NX_ROOT/build holding its output
    python_exe()   env NX_PYTHON, else base/tools/.venv, else NX_ROOT/base/tools/.venv, else the running interpreter
"""
import os
import sys

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


def python_exe():
    env = os.environ.get('NX_PYTHON')
    if env:
        return env
    for base in (NX_BASE, os.path.join(NX_ROOT, 'base')):
        p = os.path.join(base, 'tools', '.venv', 'Scripts', 'python.exe')
        if os.path.exists(p):
            return p
    return sys.executable


def game_dir(game):
    """`game` itself when it is a folder with a game.json, else games/<id> of the main checkout (NX_ROOT)."""
    if os.path.isfile(os.path.join(game, 'game.json')):
        return os.path.abspath(game)
    return os.path.join(NX_ROOT, 'games', game)
