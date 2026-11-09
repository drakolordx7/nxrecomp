// SPDX-License-Identifier: GPL-3.0-or-later
// The test tooling's side of the game boundary (docs/GAME_BOUNDARY.md): the guest memory helpers every probe uses, the generic script steps
// @peek and @poke, and the entry points tt_core.cpp calls for the game's own probes, which go to the registered game's tt::GameProbes
// (nxrt/game.h Info::probes; Metroid Prime Remastered: games/mpr/runtime/mpr_tt_probe.cpp). Without a table the frame source "hook" registers
// nothing and read_state reads nothing; the game's script steps are in the table (tt_script.cpp finds them, tt_runner.cpp runs them).
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "nxrt/game.h"
#include "nxrt/log.h"
#include "tt_internal.h"

namespace nxrt::tt {
namespace detail {
bool read_mem(uint64_t addr, void* out, size_t n) {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), out, n, &got) && got == n;
}
bool write_mem(uint64_t addr, const void* in, size_t n) {
    if (addr < 0x10000) return false;
    uint8_t probe[64];
    if (n > sizeof probe || !read_mem(addr, probe, n)) return false;
    __try {          // a direct store: WriteProcessMemory costs tens of milliseconds for an address in the guest heap (see play_aim.cpp)
        std::memcpy(reinterpret_cast<void*>(addr), in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace detail
}  // namespace nxrt::tt

namespace nxrt::tt {
namespace {

const GameProbes* P() { return game::info().probes; }

template <class T>
bool rd(uint64_t a, T* v) { return detail::read_mem(a, v, sizeof(T)); }

}  // namespace

// ---- the game's probes ----
void install_game_hooks() {
    if (const GameProbes* p = P(); p && p->install_hooks) p->install_hooks();
    else NXLOG_W("tt", "frame source hook: the game (%s) has no probes, no game hooks are registered", game::info().id);
}
bool read_state(GameState* out, ::Ctx* c) {
    const GameProbes* p = P();
    return p && p->read_state && p->read_state(out, c);
}

namespace detail {

// ---- @peek: read guest memory (S26) --------------------------------------------------------------------------------------------------------------
// in: lines `addr len [off1 off2 ...]` (hex or decimal): follow the 64-bit pointers p = *(p + off) for each off, then read len bytes at p.
// out: one line per request: the bytes as hex, or `-` when a read failed. Lets the route code poll the positions of objects it found with @objs
// without scanning memory again.
bool peek_memory(const std::string& in_path, const std::string& out_path, std::string* err) {
    std::ifstream in(in_path);
    if (!in) { *err = "cannot read " + in_path; return false; }
    FILE* out = std::fopen(out_path.c_str(), "wb");
    if (!out) { *err = "cannot write " + out_path; return false; }
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        uint64_t addr = 0, len = 0;
        if (!(ss >> std::hex >> addr >> std::dec >> len) || len == 0 || len > 4096) { std::fputs("-\n", out); continue; }
        std::string tok;
        bool ok = true;
        uint64_t p = addr;
        while (ss >> tok) {
            const uint64_t off = std::strtoull(tok.c_str(), nullptr, 0);
            uint64_t q = 0;
            if (!rd(p + off, &q) || q == 0) { ok = false; break; }
            p = q;
        }
        std::vector<uint8_t> buf(len);
        if (ok && detail::read_mem(p, buf.data(), len)) {
            for (uint8_t b : buf) std::fprintf(out, "%02x", b);
            std::fputc('\n', out);
        } else {
            std::fputs("-\n", out);
        }
        n++;
    }
    std::fclose(out);
    return true;
}


// ---- @poke: write guest memory (S26, development aid: restoring health after a death while a room is being worked out) ------------------------------
// in: lines `addr [off ...] : hexbytes`. The pointers are followed like @peek; the bytes are stored with a read-back check of the page first.
static void guarded_copy(uint64_t dst, const void* src, size_t n) {
    __try { std::memcpy(reinterpret_cast<void*>(dst), src, n); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

bool poke_memory(const std::string& in_path, std::string* err) {
    std::ifstream in(in_path);
    if (!in) { *err = "cannot read " + in_path; return false; }
    std::string line;
    while (std::getline(in, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::istringstream ss(line.substr(0, colon));
        uint64_t addr = 0;
        if (!(ss >> std::hex >> addr)) continue;
        std::string tok;
        uint64_t p = addr;
        bool ok = true;
        while (ss >> tok) {
            if (tok[0] == '+') { p += std::strtoull(tok.c_str() + 1, nullptr, 0); continue; }      // `+N`: add N without following a pointer
            uint64_t q = 0;
            if (!rd(p + std::strtoull(tok.c_str(), nullptr, 0), &q) || q == 0) { ok = false; break; }
            p = q;
        }
        std::string hex = line.substr(colon + 1);
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < hex.size(); ) {
            if (!std::isxdigit(static_cast<unsigned char>(hex[i]))) { i++; continue; }
            bytes.push_back(static_cast<uint8_t>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
            i += 2;
        }
        uint8_t probe[8];
        if (!ok || bytes.empty() || !detail::read_mem(p, probe, 1)) continue;
        guarded_copy(p, bytes.data(), bytes.size());
    }
    return true;
}

}  // namespace detail
}  // namespace nxrt::tt
