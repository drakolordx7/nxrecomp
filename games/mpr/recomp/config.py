# SPDX-License-Identifier: GPL-3.0-or-later
"""Lifting configuration of Metroid Prime Remastered (Nintendo Switch).

The game's names, title id, builds and module list (bases, shard counts) are in its manifest, game.json; the lifter reads them from
there (base recomp/nxrecomp/config.py load_game). This file holds only what changes lifting.
"""
from nxrecomp.config import GameConfig


class Config(GameConfig):
    pass
