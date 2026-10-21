# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-game configuration of Metroid Prime Remastered (Nintendo Switch, title 010012101468C000, v1.0.0).

PLACEHOLDER: the game binary has not been dumped yet, so nothing here is measured. The values below are the ones the
project's tool.md fixes (title, version, build id, image base 0x7100000000); everything else is empty on purpose.
Fill in when the dump exists (Phase 2 gate of docs/PLAN.md): module list and bases, overrides for functions the runtime
replaces (nn:: imports, hook sites), instruction patches, extra entries the discovery cannot see.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from nxrecomp.config import GameConfig, ModuleSpec          # noqa: E402


class Config(GameConfig):
    name = 'mpr'
    title_id = '010012101468C000'
    version = '1.0.0'
    build_id = '67C9CA1428222752F951021464A7B3597EB98FEC'      # of main; checked by the setup tool, not by the lifter
    # main is mapped at 0x7100000000 (docs/PLAN.md); the other modules' bases are the runtime's choice: PLACEHOLDERS
    modules = [
        ModuleSpec('main', 0x7100000000, 'main'),
        ModuleSpec('sdk', 0x7500000000, 'sdk'),
        ModuleSpec('subsdk0', 0x7600000000, 'subsdk0'),
    ]
    overrides = {}          # {guest address: handler name}: the runtime's h_<name>(Ctx *c) replaces that function
    extra_entries = {}      # {module name: [module-relative offsets]}
    patches = None
