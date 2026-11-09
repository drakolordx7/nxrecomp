"""The folders the profiling scripts work in (base docs/BUILD.md).

nx_root(): the NX folder (games/, build/): NX_ROOT, else the nearest parent of this checkout with a games/ folder.
project_root(): the game folder (its game/, work/ and manifest game.json live there): NX_GAME_DIR, else <NX>/games/<NX_GAME>.
game_exe(): the game exe of the game build, <NX>/build/<id>/<files.exe of the manifest>.
"""
import json
import os


def nx_root():
    e = os.environ.get('NX_ROOT')
    if e:
        return e
    d = os.path.dirname(os.path.abspath(__file__))
    while True:
        if os.path.isdir(os.path.join(d, 'games')):
            return d
        p = os.path.dirname(d)
        if p == d:
            return os.getcwd()
        d = p


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
    return os.path.join(nx_root(), 'build', m['id'], m['files']['exe'])
