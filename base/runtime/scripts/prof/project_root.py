"""The folders the profiling scripts work in (base docs/BUILD.md).

nx_root(): the main checkout (game data, build/, wt/): NX_ROOT, else the parent of wt/ when this checkout is a worktree, else this checkout.
project_root(): the game folder (its game/, work/ and manifest game.json live there): NX_GAME_DIR, else <NX>/games/<NX_GAME>.
game_exe(): the game exe of the game build, <NX>/build/<main | worktree name>/<id>/release/<files.exe of the manifest>.
"""
import json
import os


def nx_root():
    e = os.environ.get('NX_ROOT')
    if e:
        return e
    checkout = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..'))   # base/runtime/scripts/prof
    up = os.path.dirname(checkout)
    return os.path.dirname(up) if os.path.basename(up).lower() == 'wt' else checkout


def checkout_name():
    """main for the main checkout, else the worktree's folder name (its build folder under NX/build)."""
    checkout = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..'))
    return 'main' if os.path.normcase(checkout) == os.path.normcase(os.path.abspath(nx_root())) else os.path.basename(checkout)


def project_root():
    e = os.environ.get('NX_GAME_DIR')
    if e:
        return e
    g = os.environ.get('NX_GAME')
    if not g:
        raise SystemExit('set NX_GAME_DIR (a game folder) or NX_GAME (a game id under <NX>/games)')
    return os.path.join(nx_root(), 'games', g)


def game_exe():
    root = project_root()
    with open(os.path.join(root, 'game.json'), encoding='utf-8') as f:
        m = json.load(f)
    return os.path.join(nx_root(), 'build', checkout_name(), m['id'], 'release', m['files']['exe'])
