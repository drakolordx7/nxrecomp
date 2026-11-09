// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-ins for SDK code that a game reaches but the PC cannot run as lifted: installed by the game exe (runtime/apps/game) once the
// modules are mapped and their lifted tables registered. Nothing here is about one title; each stand-in names the SDK function it replaces.
#pragma once
#include <cstdint>

namespace nxrt {

// SDK functions replaced by host code (forced over the import and over the definition, so direct and indirect calls both reach them):
//   nvnBootstrapLoader               the runtime's NVN front end ([boot] nvn = 0 leaves the logging stubs)
//   nn::os::GetHostArgc / Argv       [boot] guest_args: the game's command line, space separated; argv[0] is the game id. Retail passes
//                                    none, so without the key the SDK's own (empty) answer stays. [<game id>] guest_args is read too.
//   nn::ngc::ProfanityFilter         initialisation succeeds and no word is flagged or masked (the console's NG-word archive is absent;
//                                    the SDK aborts with fs 2/1002 without it)
void install_sdk_standins();

}
