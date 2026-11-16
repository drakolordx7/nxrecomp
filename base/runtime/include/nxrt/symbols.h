// SPDX-License-Identifier: GPL-3.0-or-later
// Host-code symbolisation. Windows: the MSVC-format linker map (link.exe /MAP) that sits next to the exe. Linux: the ELF symbol table of the exe
// (src/os/posix/symbols.cpp; an unstripped binary) and the exported names of loaded libraries. The tables live in the os layer (nxrt/os/symbols.h).
//   Address         Publics by Value              Rva+Base               Lib:Object
//   0001:00000010       ?name@@YAXXZ              0000000140001010 f   file.obj
// Used by the crash handler and the watchdog to name host frames without a PDB.
#pragma once
#include <cstdint>
#include <string>

namespace nxrt {

struct SymbolInfo {
    std::string module;   // "nxrt_tests.exe" (file name only), or "?" when the address is in no loaded module
    uint64_t offset = 0;
    std::string symbol;   // nearest preceding public symbol (undecorated when possible), empty when unknown
    uint64_t sym_offset = 0;
    bool from_map = false;
};

// Loads the symbol table of the main exe: Windows <exe path>.map's "Publics by Value" section (without an argument: the map next to the
// running exe, same name, .map extension); Linux the ELF symtab of the file given, default the running exe. Returns the number of symbols
// loaded (0 = none found / unreadable).
size_t symbols_load_map(const char* map_path = nullptr);
// Windows: the same for map text (tests; the Rva+Base column assumes the preferred base). Linux: no such text, returns 0 (os::symbols::text_tables_supported()).
size_t symbols_load_map_text(const std::string& text);
size_t symbols_count();
// Resolve a host code address. Never throws, never allocates beyond the returned strings, safe to call from the crash handler
// only when symbols_load_map has run earlier (the lookup itself allocates strings: the crash handler tolerates that).
SymbolInfo symbols_lookup(uint64_t host_addr);
// "module+0xOFFSET (symbol+0xN)" or "module+0xOFFSET" or "0xADDR".
std::string symbols_describe(uint64_t host_addr);

}
