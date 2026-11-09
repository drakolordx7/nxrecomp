# SPDX-License-Identifier: GPL-3.0-or-later
"""The per-game configuration seam.

Everything game-specific that the generic lifter must not know lives in a `GameConfig` subclass under
recomp/games/<name>/config.py (and is selected with --game <name>). smo-recomp kept this knowledge inside its
lifter (sdklift_rules.txt, "nerve" RTTI naming, NPDM / version checks, base-to-update symbol matching); here the
lifter only asks the config. The default config is a neutral one that lifts a module as it is.
"""
import importlib
import importlib.util
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
    """The GameConfig instance of recomp/games/<name>/config.py (class `Config`)."""
    if not name or name == 'generic':
        return GameConfig()
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(here, 'games', name, 'config.py')
    spec = importlib.util.spec_from_file_location('nxrecomp_game_' + name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.Config()
