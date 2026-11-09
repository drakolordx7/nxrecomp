// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered: the boot hook (nxrt/game.h Info::boot), run by the game exe once the modules are mapped and registered, before
// they are linked. Only a diagnostic so far.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "mpr_game.h"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/log.h"

namespace nxrt::games::mpr {

namespace {

// TEMP debug 121 ([boot] dump_killer = 1): dump the killer slots at a fault
LONG CALLBACK dump_killer(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;
    static int once = 0;
    if (once++) return EXCEPTION_CONTINUE_SEARCH;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(0x710455d5b8ull);
    for (int slot = 0; slot < 2; slot++) {
        const uint8_t* k = base + 0x100 + slot * 0xd8;
        const uint64_t ids = *reinterpret_cast<const uint64_t*>(k + 0x58);
        const uint32_t n = *reinterpret_cast<const uint32_t*>(k + 0x50);
        NXLOG_E("dbg", "killer slot %d: tex ids ptr 0x%llx count %u (cap %u)", slot, (unsigned long long)ids, n, *reinterpret_cast<const uint32_t*>(k + 0x54));
        if (ids) { std::string t; for (uint32_t i = 0; i < n && i < 64; i++) { char b[16]; std::snprintf(b, 16, "%x ", reinterpret_cast<const uint32_t*>(ids)[i]); t += b; } NXLOG_E("dbg", "  ids: %s", t.c_str()); }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI killer_poll(LPVOID) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(0x710455d5b8ull);
    uint32_t last[2] = {0, 0};
    for (;;) {
        for (int slot = 0; slot < 2; slot++) {
            const uint32_t n = *reinterpret_cast<const volatile uint32_t*>(base + 0x100 + slot * 0xd8 + 0x50);
            if (n != last[slot]) {
                if (n > last[slot] && last[slot] == 0) {
                    std::string t;
                    for (const auto& r : nxrt::diag_threads()) {
                        if (!r.ctx) continue;
                        char b[96]; std::snprintf(b, sizeof b, "\n  %s fn=%llx lr=%llx:", r.name.c_str(), (unsigned long long)r.ctx->fn, (unsigned long long)r.ctx->x[30]); t += b;
                        uint64_t f = r.ctx->x[29];
                        for (int d = 0; d < 10 && f > 0x1000 && !(f & 7); d++) { std::snprintf(b, sizeof b, " %llx", (unsigned long long)*reinterpret_cast<const uint64_t*>(f + 8)); t += b; f = *reinterpret_cast<const uint64_t*>(f); }
                    }
                    NXLOG_E("dbg", "killer slot %d count %u -> %u%s", slot, last[slot], n, t.c_str());
                }
                last[slot] = n;
            }
        }
    }
    return 0;
}

}

bool boot(std::string&) {
    if (nxrt::config_get_bool("boot", "dump_killer", false)) { AddVectoredExceptionHandler(1, dump_killer); CreateThread(nullptr, 0, killer_poll, nullptr, 0, nullptr); }
    return true;
}

}
