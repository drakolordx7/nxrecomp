// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-ins for SDK code (nxrt/sdk_hle.h).
#include "nxrt/sdk_hle.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"

namespace nxrt {
namespace {

void force_to_host(const char* sym, HleFn fn) {
    module_override_symbol(sym);
    if (const uint64_t def = nxmodule_find_export(sym)) fn_register(def, fn);
}

// ---- nn::os::GetHostArgc / GetHostArgv ----------------------------------------------------------------------------------------------
std::vector<std::string> g_args;
std::vector<uint64_t> g_argv;

void install_guest_args() {
    const game::Info& g = game::info();
    std::string list = config_get_str("boot", "guest_args", "");
    if (list.empty() && *g.id) list = config_get_str(g.id, "guest_args", "");
    if (list.empty()) return;
    g_args.push_back(*g.id ? g.id : "game");
    for (size_t p = 0; p < list.size();) {
        size_t e = list.find(' ', p);
        if (e == std::string::npos) e = list.size();
        if (e > p) g_args.push_back(list.substr(p, e - p));
        p = e + 1;
    }
    for (const std::string& a : g_args) g_argv.push_back(reinterpret_cast<uint64_t>(a.c_str()));
    g_argv.push_back(0);
    const char* argc_sym = "_ZN2nn2os11GetHostArgcEv";
    const char* argv_sym = "_ZN2nn2os11GetHostArgvEv";
    hle_register(argc_sym, [](::Ctx* c) { c->x[0] = g_args.size(); });
    hle_register(argv_sym, [](::Ctx* c) { c->x[0] = reinterpret_cast<uint64_t>(g_argv.data()); });
    for (const char* sym : {argc_sym, argv_sym})
        if (HleFn fn = hle_lookup(sym)) force_to_host(sym, fn);
    NXLOG_I("boot", "guest command line: %s", list.c_str());
}

// ---- nn::ngc::ProfanityFilter --------------------------------------------------------------------------------------------------------
void install_profanity_filter() {
    struct Stub { const char* sym; HleFn fn; };
    static const Stub stubs[] = {
        {"_ZN2nn3ngc15ProfanityFilterC1Ev", [](::Ctx*) {}},
        {"_ZN2nn3ngc15ProfanityFilterD1Ev", [](::Ctx*) {}},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", [](::Ctx* c) { c->x[0] = 0; }},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", [](::Ctx* c) {   // (this, u32* out, u32 mask, words, count)
            if (uint32_t* out = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(c->x[1])))
                for (uint64_t i = 0; i < c->x[4] && i < 4096; ++i) out[i] = 0;
            c->x[0] = 0;
        }},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", [](::Ctx* c) {   // (this, int* count, char* text, u32 mask)
            if (int32_t* n = reinterpret_cast<int32_t*>(static_cast<uintptr_t>(c->x[1]))) *n = 0;
            c->x[0] = 0;
        }},
    };
    for (const Stub& st : stubs) {
        hle_register(st.sym, st.fn);
        force_to_host(st.sym, st.fn);
    }
}

}

void install_sdk_standins() {
    // The game reaches NVN only through `nvnBootstrapLoader` (an import of main that the sdk defines: Nintendo's driver). The lifted PLT stub
    // of main calls host_import(), not the GOT that rtld fills, so the symbol is forced to the runtime's NVN front end; the sdk's own
    // definition is also replaced in the call table for any indirect call that reaches it by address.
    if (config_get_bool("boot", "nvn", true)) {
        if (HleFn fn = hle_lookup("nvnBootstrapLoader")) force_to_host("nvnBootstrapLoader", fn);
        else module_override_symbol("nvnBootstrapLoader");
    }
    install_guest_args();
    install_profanity_filter();
}

}
