# SPDX-License-Identifier: GPL-3.0-or-later
"""Jump-table resolution for `br xN` (placeholder: resolves nothing yet, so every `br` uses the dispatch fallback)."""


def resolve(env):
    env.jt = {}
