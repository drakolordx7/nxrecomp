# SPDX-License-Identifier: GPL-3.0-or-later
"""The per-game configuration seam.

Everything game-specific that the generic lifter must not know lives in a `GameConfig` subclass in the game's folder,
games/<id>/recomp/config.py (selected with --game <path of that file>). The game's names, ids, module list and bases are in its
manifest (games/<id>/game.json), which the build passes to the lifter as --base, --index and --sites; the config holds only what changes
lifting. smo-recomp kept this knowledge inside its
lifter (sdklift_rules.txt, "nerve" RTTI naming, NPDM / version checks, base-to-update symbol matching); here the
lifter only asks the config. The default config is a neutral one that lifts a module as it is.
"""
import importlib.util
import os


class GameConfig:
    name = 'generic'


def load_game(name):
    """The GameConfig instance of a game's config.py (class `Config`): `name` is the path of that file (games/<id>/recomp/config.py),
    or empty / 'generic' for the neutral config."""
    if not name or name == 'generic':
        return GameConfig()
    path = os.path.abspath(name)
    if os.path.isdir(path):
        path = os.path.join(path, 'config.py')
    if not os.path.isfile(path):
        raise FileNotFoundError(f"game config {name}: no such file (pass the path of a game's recomp/config.py)")
    spec = importlib.util.spec_from_file_location('nxrecomp_game_config', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.Config()
