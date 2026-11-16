// SPDX-License-Identifier: GPL-3.0-or-later
// Host-code symbolisation (declared in nxrt/symbols.h): the portable half. The tables and the module lookup are the host's own and live in
// src/os/win32/symbols.cpp (linker map) and src/os/posix/symbols.cpp (ELF symtab); this file keeps the public API and the text format.
#include "nxrt/symbols.h"

#include <cstdio>

#include "nxrt/os/symbols.h"

namespace nxrt {
namespace {

constexpr size_t kMaxDescribeName = 120;

std::string hex64(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
    return b;
}

}

size_t symbols_load_map(const char* map_path) { return os::symbols::load(map_path); }

size_t symbols_load_map_text(const std::string& text) { return os::symbols::load_text(text); }

size_t symbols_count() { return os::symbols::count(); }

SymbolInfo symbols_lookup(uint64_t host_addr) {
    os::symbols::Info i = os::symbols::lookup(host_addr);
    SymbolInfo info;
    info.module = std::move(i.module);
    info.offset = i.offset;
    info.symbol = std::move(i.symbol);
    info.sym_offset = i.sym_offset;
    info.from_map = i.from_table;
    return info;
}

std::string symbols_describe(uint64_t host_addr) {
    SymbolInfo s = symbols_lookup(host_addr);
    if (s.module == "?") return "0x" + hex64(host_addr);
    std::string out = s.module + "+0x" + hex64(s.offset);
    if (!s.symbol.empty()) {
        std::string name = s.symbol.size() > kMaxDescribeName ? s.symbol.substr(0, kMaxDescribeName) : s.symbol;
        out += " (" + name + "+0x" + hex64(s.sym_offset) + ")";
    }
    return out;
}

}
