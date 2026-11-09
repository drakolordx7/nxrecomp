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

// Forces the import of `sym` to the host handler and replaces the SDK's own definition in the call table (an indirect call that reaches
// it by address).
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

// ---- rtld ------------------------------------------------------------------------------------------------------------------------------
// The rtld build these stand-ins were written from, instruction for instruction (disassembled from a dump):
//   0x08  cmp x0, #0 ; b.ne 0xd8              x0 != 0: the kernel's entry for a user exception (not handled here)
//   0x10  w19 = w1                            main thread handle
//   0x14  memset(bss, 0, size)                already zero in the fresh mapping
//   0x48  x0 = module base ; x1 = &_DYNAMIC   (0x3010)
//   0x78  bl 0x13c0 (x0, x1)                  rtld's self-relocation (RELATIVE entries, addend based: idempotent after the host link)
//   0x84  bl 0x04b0 (x0, x1)                  the dynamic linker: finds the other modules with svcQueryMemory, relocates them
//   0x88  nn::init::Start(w19, GOT[0x3288], base+0x138c, base+0x820, base+0x870)   (bl 0x1700 at 0xd0, the PLT stub); never returns
//   0x138c mov x0, x30 ; bl 0x1398             0x1398: x2 = 0x1394 + [0x1394] = rtld+0x3008 ; [x2] = 1 ; x30 = x0 ; ret
// It is recognised by those branch words; another rtld build has other offsets and is left alone.
uint64_t g_rtld = 0;

uint32_t word(uint64_t a) { return *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(a)); }
bool known_rtld(uint64_t b) {
    return word(b + 0x78) == 0x940004d2u && word(b + 0x84) == 0x9400010bu && word(b + 0xd0) == 0x9400058cu && word(b + 0x138c) == 0xaa1e03e0u &&
           word(b + 0x1390) == 0x94000002u;
}

// Diagnostic probe, [boot] probe = no-sdk-services: nn::init::Start (sdk) written out step by step, minus the one step that talks to Horizon
// services (nninitInitializeSdkModule: lm, appletOE, ...), so the rest of the start-up can be exercised before the service library exists.
// The order is the one in Start's disassembly:
//   _init_libc0 ; nnosInitialize(handle, exception handler) ; rtld+0x138c ; _init_libc1 ; [nninitInitializeSdkModule] ; nninitStartup ;
//   _init_libc2 ; rtld+0x820 (every module's constructors) ; nnMain ; rtld+0x870 ; nninitFinalizeSdkModule ; nnosQuickExit
void call_named(::Ctx* c, const char* name) {
    const uint64_t a = nxmodule_find_export(name);
    if (!a) { NXLOG_E("boot", "probe: no module defines %s", name); trap("probe: missing function", 0); }
    NXLOG_I("boot", "probe: %s at 0x%llx", name, static_cast<unsigned long long>(a));
    dispatch_call(c, a);
}
void probe_start(::Ctx* c, uint32_t handle, uint64_t exception_handler) {
    const uint64_t b = g_rtld;
    NXLOG_W("boot", "PROBE no-sdk-services: nn::init::Start is replaced by its steps without nninitInitializeSdkModule");
    call_named(c, "_init_libc0");
    c->x[0] = handle; c->x[1] = exception_handler;
    call_named(c, "nnosInitialize");
    dispatch_call(c, b + 0x138c);
    call_named(c, "_init_libc1");
    call_named(c, "nninitStartup");
    call_named(c, "_init_libc2");
    dispatch_call(c, b + 0x820);
    call_named(c, "nnMain");
    dispatch_call(c, b + 0x870);
    call_named(c, "nninitFinalizeSdkModule");
    call_named(c, "nnosQuickExit");
}

void rtld_entry(::Ctx* c) {
    const uint64_t b = g_rtld;
    if (c->x[0] != 0) {
        NXLOG_E("boot", "rtld entered for a user exception (x0 = 0x%llx): not implemented", static_cast<unsigned long long>(c->x[0]));
        trap("rtld exception entry", c->x[0]);
    }
    const uint32_t handle = static_cast<uint32_t>(c->x[1]);
    c->x[0] = b;
    c->x[1] = b + 0x3010;
    dispatch_call(c, b + 0x13c0);
    if (config_get_str("boot", "link", "rtld") != "host") {
        c->x[0] = b;
        c->x[1] = b + 0x3010;
        dispatch_call(c, b + 0x4b0);
    }
    if (config_get_str("boot", "probe", "") == "no-sdk-services") {
        probe_start(c, handle, *reinterpret_cast<const uint64_t*>(b + 0x3288));
        return;
    }
    c->x[0] = handle;
    c->x[1] = *reinterpret_cast<const uint64_t*>(b + 0x3288);
    c->x[2] = b + 0x138c;
    c->x[3] = b + 0x820;
    c->x[4] = b + 0x870;
    dispatch_call(c, b + 0x1700);
    NXLOG_E("boot", "nn::init::Start returned (x0 = 0x%llx): rtld would loop here", static_cast<unsigned long long>(c->x[0]));
    trap("nn::init::Start returned", c->x[0]);
}

void rtld_fn_138c(::Ctx*) { *reinterpret_cast<uint32_t*>(g_rtld + 0x3008) = 1; }

// Diagnostic ([boot] trace_plt = 1): wraps rtld's lazy PLT resolver (rtld+0x1318) and logs every binding: the slot, the link map, the
// relocation index and what the lifted lookup (rtld+0x8b0) answers, before the resolver runs for real.
GuestFn g_orig_resolver = nullptr;
void traced_resolver(::Ctx* c) {
    const uint64_t slot = *reinterpret_cast<const uint64_t*>(c->x[31]);     // x16 pushed by PLT0
    const uint64_t got2 = c->x[16];
    const uint64_t idx = (slot - got2 - 8) / 8;
    const uint64_t lm = *reinterpret_cast<const uint64_t*>(got2 - 8);
    ::Ctx tmp = *c;
    tmp.x[0] = lm; tmp.x[1] = idx; tmp.x[17] = slot; tmp.x[30] = 0;
    dispatch_call(&tmp, g_rtld + 0x8b0);
    NXLOG_I("boot", "plt resolve: slot=0x%llx got2=0x%llx lm=0x%llx idx=%llu -> 0x%llx (GOT slot now 0x%llx)", (unsigned long long)slot,
            (unsigned long long)got2, (unsigned long long)lm, (unsigned long long)idx, (unsigned long long)tmp.x[0],
            (unsigned long long)*reinterpret_cast<const uint64_t*>(slot));
    g_orig_resolver(c);
}

}  // namespace

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

bool install_rtld_standins(uint64_t rtld_base) {
    const bool known = known_rtld(rtld_base);
    if (known) {
        g_rtld = rtld_base;
        if (!fn_lookup(rtld_base)) {
            fn_register(rtld_base, rtld_entry);
            NXLOG_W("boot", "the lifted rtld has no function at its entry: using the host stand-in (sdk_standins.cpp rtld_entry)");
        }
        if (!fn_lookup(rtld_base + 0x138c)) {
            fn_register(rtld_base + 0x138c, rtld_fn_138c);
            NXLOG_W("boot", "the lifted rtld has no function at 0x138c: using the host stand-in (sdk_standins.cpp rtld_fn_138c)");
        }
        if (config_get_bool("boot", "trace_plt", false)) {
            g_orig_resolver = fn_lookup(rtld_base + 0x1318);
            if (g_orig_resolver) fn_register(rtld_base + 0x1318, traced_resolver);
        }
    }
    return fn_lookup(rtld_base) != nullptr;
}

}  // namespace nxrt
