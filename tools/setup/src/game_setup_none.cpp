// SPDX-License-Identifier: GPL-3.0-or-later
// The setup tool of a game that has no hook (no "setup_hook" in its manifest): extraction only.
#include "game_setup.h"

namespace nxs {

const GameSetup* game_setup() { return nullptr; }

}  // namespace nxs
