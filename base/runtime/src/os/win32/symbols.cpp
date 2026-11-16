// SPDX-License-Identifier: GPL-3.0-or-later
// Windows implementation of nxrt/os/symbols.h. Host-code symbolisation from the MSVC-format linker map that lld-link writes next to the exe
// (/MAP): reads the "Publics by Value" and "Static symbols" entries, undecorates the MSVC names with dbghelp and resolves a host address to the
// nearest preceding symbol. Only the main exe has a table; other modules are reported by name and offset. Stacks are walked with
// RtlLookupFunctionEntry / RtlVirtualUnwind (pattern from smo-recomp runtime/core/hostrt.c crash_filter); the walk moved here from
// diag/stackwalk.cpp and is shared with src/os/win32/thread.cpp through walk.h.
#include "nxrt/os/symbols.h"

#include "walk.h"

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace nxrt::os::symbols {
namespace {

constexpr uint64_t kDefaultPreferredBase = 0x140000000ull;  // MSVC default exe base, used when the map has no such line
constexpr uint64_t kMaxSymbolDistance = 1ull << 20;

struct Entry {
    uint64_t rva;      // relative to the image base the map was written for
    std::string name;
};

struct Table {
    std::vector<Entry> entries;
};

// Copy-on-write: a load builds a new table and swaps the pointer; readers keep the table they copied.
std::shared_mutex g_table_mu;
std::shared_ptr<const Table> g_table = std::make_shared<Table>();
// dbghelp is not thread safe.
std::mutex g_dbghelp_mu;

std::shared_ptr<const Table> current_table() {
    std::shared_lock lk(g_table_mu);
    return g_table;
}

void install_table(std::shared_ptr<const Table> t) {
    std::unique_lock lk(g_table_mu);
    g_table = std::move(t);
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

struct Token {
    size_t pos;
    size_t len;
};

std::vector<Token> tokenize(std::string_view line) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && is_space(line[i])) ++i;
        size_t start = i;
        while (i < line.size() && !is_space(line[i])) ++i;
        if (i > start) out.push_back({start, i - start});
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_hex(std::string_view s, uint64_t& out) {
    if (s.empty() || s.size() > 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        int d = hex_value(c);
        if (d < 0) return false;
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    out = v;
    return true;
}

// "XXXX:XXXXXXXX" as written in the map (segment:offset).
bool parse_seg_off(std::string_view s, unsigned& seg) {
    if (s.size() != 13 || s[4] != ':') return false;
    uint64_t seg64 = 0, off = 0;
    if (!parse_hex(s.substr(0, 4), seg64) || !parse_hex(s.substr(5, 8), off)) return false;
    seg = static_cast<unsigned>(seg64);
    return true;
}

std::string undecorate(const std::string& raw) {
    if (raw.size() < 2 || raw[0] != '?') return raw;
    constexpr DWORD kFlags = UNDNAME_NO_ACCESS_SPECIFIERS | UNDNAME_NO_MEMBER_TYPE | UNDNAME_NO_ALLOCATION_LANGUAGE;
    char buf[4096];
    std::lock_guard lk(g_dbghelp_mu);
    DWORD n = UnDecorateSymbolName(raw.c_str(), buf, sizeof buf, kFlags);
    if (n == 0) return raw;
    buf[sizeof buf - 1] = '\0';
    return buf;
}

struct RawSymbol {
    unsigned seg;
    uint64_t abs;  // the Rva+Base column as written (runtime address at the preferred base)
    std::string name;
};

struct ParsedMap {
    uint64_t preferred = kDefaultPreferredBase;
    bool have_sections = false;
    std::map<unsigned, std::string> seg_class;  // segment number -> class (CODE, DATA, ...), from the section table
    std::vector<RawSymbol> raw;
};

// One line of the symbol tables: "<seg>:<off>  <name...>  <16 hex Rva+Base>  <Lib:Object>". Section-table rows
// ("<seg>:<off>  <len>H  <section>  <class>") are recorded in pm.seg_class. Anything else is ignored.
void parse_line(std::string_view line, ParsedMap& pm) {
    auto toks = tokenize(line);
    if (toks.size() < 4) return;
    auto tok = [&](size_t k) { return line.substr(toks[k].pos, toks[k].len); };
    unsigned seg = 0;
    if (!parse_seg_off(tok(0), seg)) return;
    const size_t n = toks.size();

    uint64_t abs = 0;
    if (tok(n - 2).size() == 16 && parse_hex(tok(n - 2), abs)) {
        size_t name_begin = toks[0].pos + toks[0].len;
        size_t name_end = toks[n - 2].pos;
        std::string_view name = trim(line.substr(name_begin, name_end - name_begin));
        if (name.empty()) return;
        pm.raw.push_back({seg, abs, std::string(name)});
        return;
    }

    uint64_t len = 0;
    if (n == 4 && tok(1).size() == 9 && tok(1)[8] == 'H' && parse_hex(tok(1).substr(0, 8), len)) {
        pm.seg_class[seg] = std::string(tok(3));
        pm.have_sections = true;
    }
}

ParsedMap parse_map_text(std::string_view text) {
    ParsedMap pm;
    constexpr std::string_view kPref = "Preferred load address is ";
    if (size_t p = text.find(kPref); p != std::string_view::npos) {
        uint64_t v = 0;
        size_t i = p + kPref.size(), digits = 0;
        while (i < text.size() && digits < 16 && hex_value(text[i]) >= 0) {
            v = (v << 4) | static_cast<uint64_t>(hex_value(text[i]));
            ++i;
            ++digits;
        }
        if (digits) pm.preferred = v;
    }
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        parse_line(text.substr(pos, eol - pos), pm);
        pos = eol + 1;
    }
    return pm;
}

std::shared_ptr<Table> build_table(const ParsedMap& pm) {
    auto t = std::make_shared<Table>();
    for (const auto& r : pm.raw) {
        if (r.seg == 0) continue;  // absolute / linker-defined
        if (pm.have_sections) {
            auto it = pm.seg_class.find(r.seg);
            if (it == pm.seg_class.end() || it->second != "CODE") continue;  // not in the executable image
        }
        if (r.abs < pm.preferred) continue;
        uint64_t rva = r.abs - pm.preferred;
        if (rva == 0) continue;
        t->entries.push_back({rva, undecorate(r.name)});
    }
    std::sort(t->entries.begin(), t->entries.end(),
              [](const Entry& a, const Entry& b) { return a.rva < b.rva; });
    return t;
}

std::string default_map_path() {
    char buf[1024];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) return {};
    std::string p(buf, n);
    size_t slash = p.find_last_of("\\/");
    size_t dot = p.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) p.erase(dot);
    return p + ".map";
}

}

size_t load(const char* map_path) {
    std::string path = map_path ? std::string(map_path) : default_map_path();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        install_table(std::make_shared<Table>());
        return 0;
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_text(text);
}

bool text_tables_supported() { return true; }

size_t load_text(const std::string& text) {
    std::shared_ptr<Table> t = build_table(parse_map_text(text));
    size_t n = t->entries.size();
    install_table(std::move(t));
    return n;
}

size_t count() { return current_table()->entries.size(); }

void prepare() {}

uint64_t main_base() { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr))); }

std::string main_name() {
    char path[1024];
    DWORD n = GetModuleFileNameA(nullptr, path, sizeof path);
    std::string full = n ? std::string(path, n) : std::string();
    size_t slash = full.find_last_of("\\/");
    return slash == std::string::npos ? full : full.substr(slash + 1);
}

std::vector<Module> modules() {
    std::vector<Module> out;
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return out;
    const size_t n = std::min<size_t>(need / sizeof(HMODULE), 1024);
    for (size_t i = 0; i < n; ++i) {
        MODULEINFO mi{};
        char path[1024];
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
        const DWORD len = GetModuleFileNameA(mods[i], path, sizeof path);
        std::string full(path, len);
        const size_t slash = full.find_last_of("\\/");
        Module m;
        m.name = slash == std::string::npos ? full : full.substr(slash + 1);
        m.base = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mi.lpBaseOfDll));
        m.size = mi.SizeOfImage;
        out.push_back(std::move(m));
    }
    const uint64_t mb = main_base();
    std::stable_sort(out.begin(), out.end(), [mb](const Module& a, const Module& b) { return (a.base == mb) > (b.base == mb); });
    return out;
}

Info lookup(uint64_t host_addr) {
    Info info;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(host_addr)), &mod) ||
        mod == nullptr) {
        info.module = "?";
        info.offset = host_addr;
        return info;
    }

    char path[1024];
    DWORD n = GetModuleFileNameA(mod, path, sizeof path);
    std::string full = n ? std::string(path, n) : std::string();
    size_t slash = full.find_last_of("\\/");
    info.module = slash == std::string::npos ? full : full.substr(slash + 1);
    if (info.module.empty()) info.module = "?";

    uint64_t base = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mod));
    info.offset = host_addr - base;
    if (mod != GetModuleHandleA(nullptr)) return info;

    auto table = current_table();
    const auto& es = table->entries;
    auto it = std::upper_bound(es.begin(), es.end(), info.offset,
                               [](uint64_t v, const Entry& e) { return v < e.rva; });
    if (it == es.begin()) return info;
    --it;
    uint64_t dist = info.offset - it->rva;
    if (dist > kMaxSymbolDistance) return info;
    info.symbol = it->name;
    info.sym_offset = dist;
    info.from_table = true;
    return info;
}

// ---- stacks -------------------------------------------------------------------------------------------------------------------------

size_t capture_current(uint64_t* frames, size_t max, size_t skip) {
    void* raw[128];
    const size_t want = std::min<size_t>(max + skip + 1, 128);
    const USHORT got = RtlCaptureStackBackTrace(0, static_cast<DWORD>(want), raw, nullptr);
    size_t n = 0;
    for (size_t i = 1 + skip; i < got && n < max; ++i) frames[n++] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(raw[i]));   // [0] is this function
    return n;
}

size_t capture_fault(const FaultInfo& fi, uint64_t* frames, size_t max) {
    const auto* ep = static_cast<const EXCEPTION_POINTERS*>(fi.native);
    if (!ep || !ep->ContextRecord) return 0;
    return detail::capture_context(ep->ContextRecord, frames, max);
}

}  // namespace nxrt::os::symbols

namespace nxrt::os::detail {

size_t capture_context(const CONTEXT* ctx, uint64_t* frames, size_t max) {
    CONTEXT c = *ctx;
    size_t n = 0;
    while (n < max && c.Rip) {
        frames[n++] = c.Rip;
        DWORD64 image_base = 0;
        RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
        if (!rf) {  // leaf function without unwind info: the return address is at [rsp]
            if (c.Rsp < 0x10000) break;
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            if (n > 1) break;   // only the first frame may be a leaf; anything else means we lost the chain
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, rf, &c, &handler_data, &establisher, nullptr);
    }
    return n;
}

}  // namespace nxrt::os::detail
