# SPDX-License-Identifier: GPL-3.0-or-later
"""Lifting configuration of Metroid Prime Remastered (Nintendo Switch).

The game's names, title id, builds and module list (bases, shard counts) are in its manifest, game.json; the lifter reads them from
there (base recomp/nxrecomp/config.py load_game). This file holds only what changes lifting.
"""
from nxrecomp.config import GameConfig


class Config(GameConfig):
    # {module name: [module-relative offsets]}. rtld's process entry (0x0) is rooted by the lifter itself.
    # rtld+0x138c (`mov x0, x30; bl 0x1398`; 0x1398 sets the flag at rtld+0x3008 and returns to the original caller) is reached only
    # through the pointer rtld hands nn::init::Start, so discovery does not see it.
    extra_entries = {'rtld': [0x138c]}
