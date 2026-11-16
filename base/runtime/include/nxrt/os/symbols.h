// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::symbols: host code addresses to names, and the host call stack. Windows (src/os/win32/symbols.cpp): the MSVC-format linker map that
// lld-link writes next to the exe (/MAP), undecorated with dbghelp, and RtlLookupFunctionEntry / RtlVirtualUnwind for stacks. Linux
// (src/os/posix/symbols.cpp): the .symtab of the exe (an unstripped binary; demangled with the C++ ABI), dladdr for other modules, and the
// unwinder of libgcc (.eh_frame) for stacks. The portable half (the "module+0xOFFSET (symbol+0xN)" text, the public nxrt/symbols.h) is in
// src/diag/symbols.cpp. See README.md in this folder for the rules of the layer.
//
// Offsets are relative to the module's load address (Windows: the image base; Linux: the lowest mapped address of the file), so a map or a
// symbol table made for the file stays valid at any load address.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nxrt/os/fault.h"

namespace nxrt::os::symbols {

struct Info {
    std::string module;      // file name of the module that holds the address ("nxrt_tests.exe", "libc.so.6"); "?" when no loaded module does
    uint64_t offset = 0;     // address minus the module's load address (the address itself when module is "?")
    std::string symbol;      // nearest preceding symbol, undecorated / demangled; empty when unknown
    uint64_t sym_offset = 0;
    bool from_table = false; // the symbol comes from the table of the main exe (map or ELF symtab), not from a loaded library's export list
};

// Names an address. Never throws. Windows: only the main exe has a table, other modules come back with module and offset. Linux: the main exe
// from its symtab (loaded on first use when load() has not run), other modules by their exported names (dladdr) where they have one.
// Allocates strings: the crash reporter tolerates that; call prepare() early so a lookup inside a fault handler does no heavy work.
Info lookup(uint64_t addr);

// Loads the symbol table of the main exe and returns the number of symbols (0: none found, lookups then name the module only).
// Windows: the linker map at `path`, default "<exe path without extension>.map". Linux: the ELF file at `path`, default /proc/self/exe (a
// different file serves a split-off debug file; the exe's load address is assumed).
size_t load(const char* path = nullptr);
// Windows: parses map text instead of a file (tests). Elsewhere there is no such text and it returns 0: ask text_tables_supported() first.
bool text_tables_supported();
size_t load_text(const std::string& text);
size_t count();
// Makes later lookups cheap and gentle: loads the main exe's table when nothing did, and learns the loaded modules. Safe to call again.
void prepare();

// The main exe: file name ("nxrt_tests.exe", "nxrt_tests") and load address.
std::string main_name();
uint64_t main_base();

struct Module {
    std::string name;       // file name
    uint64_t base = 0;      // load address
    uint64_t size = 0;      // span from base to the end of the last mapped segment
};
std::vector<Module> modules();     // the loaded modules, the main exe first

// ---- host call stacks ---------------------------------------------------------------------------------------------------------------
// Return addresses of the calling thread, innermost first (frames[0] is a place inside the function that called capture_current when skip is 0);
// `skip` drops that many innermost frames. Not for use inside a fault handler (use capture_fault there). Returns the number written.
size_t capture_current(uint64_t* frames, size_t max, size_t skip = 0);
// The stack of the thread that faulted, from inside the fault handler that received `fi` (frames[0] is fi.pc). Windows: from the exception's context.
// Linux: from the signal context, or, for a guest trap (no context), from the stack of the calling thread starting at the frame that raised
// it. Allocation free on Windows; on Linux it uses libgcc's unwinder, which does not allocate either. Returns the number written.
size_t capture_fault(const FaultInfo& fi, uint64_t* frames, size_t max);

}  // namespace nxrt::os::symbols
