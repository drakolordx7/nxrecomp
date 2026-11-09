// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-ins for SDK code that a game reaches but the PC cannot run as lifted: installed by the game exe (runtime/apps/game) once the
// modules are mapped and their lifted tables registered. Nothing here is about one title; each stand-in names the SDK function or the
// rtld code it replaces.
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

// The process entry of the SDK's rtld build whose code is recognised at `rtld_base` (the instruction words are checked, so another rtld
// build is left alone). Two pieces of it have no lifted function in a lift without entry points for them:
//   the entry (offset 0)              straight-line code: self-relocation, the dynamic linker, nn::init::Start
//   offset 0x138c                     `mov x0, x30 ; bl 0x1398`: stores 1 in a flag word and returns to its caller
// Each is registered only when the lifted table has no function there. Returns false when rtld has no entry function and none could be
// put there (the game cannot start). Diagnostics of the same build: [boot] trace_plt = 1 logs every lazy PLT binding,
// [boot] probe = no-sdk-services runs nn::init::Start's steps without nninitInitializeSdkModule.
bool install_rtld_standins(uint64_t rtld_base);

}
