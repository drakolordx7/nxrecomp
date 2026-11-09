# SPDX-License-Identifier: GPL-3.0-or-later
"""The per-game configuration seam.

Everything game-specific that the generic lifter must not know lives in a `GameConfig` subclass in the game's folder,
games/<id>/recomp/config.py (selected with --game <path of that file>). The game's names, ids, module list and bases are in its
manifest (games/<id>/game.json); the config holds only what changes lifting. smo-recomp kept this knowledge inside its
lifter (sdklift_rules.txt, "nerve" RTTI naming, NPDM / version checks, base-to-update symbol matching); here the
lifter only asks the config. The default config is a neutral one that lifts a module as it is.
"""
import importlib
import importlib.util
import json
import os


class ModuleSpec:
    """One module of the program: its name ('main', 'sdk', 'subsdk0', ...) and the guest address of its first byte."""

    def __init__(self, name, base):
        self.name = name
        self.base = base


class GameConfig:
    name = 'generic'

    # modules of the program, in load order
    modules = []                      # [ModuleSpec]

    # guest address -> name of a host handler `h_<name>` that REPLACES the guest function (called as h_<name>(Ctx *c)).
    # Direct calls, indirect calls (call_indirect) and tail calls to that address all go to the handler.
    overrides = {}

    # extra function entries the discovery cannot see (offsets are module-relative), per module name. rtld's process entry
    # (offset 0) is rooted by the lifter itself.
    extra_entries = {}


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
    cfg = mod.Config()
    manifest = os.path.join(os.path.dirname(os.path.dirname(path)), 'game.json')
    if os.path.isfile(manifest):
        apply_manifest(cfg, manifest)
    return cfg


def apply_manifest(cfg, path):
    """Fill the config's name and module list from the game's manifest (games/<id>/game.json), unless the config sets them."""
    with open(path, encoding='utf-8') as f:
        m = json.load(f)
    if 'name' not in type(cfg).__dict__:
        cfg.name = m['id']
    if not cfg.modules:
        cfg.modules = [ModuleSpec(x['name'], int(x['base'], 16)) for x in m.get('modules', [])]
