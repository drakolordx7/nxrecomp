// SPDX-License-Identifier: GPL-3.0-or-later
// Linux implementation of nxrt/os/symbols.h. Names: the .symtab of the exe (functions only, read once from /proc/self/exe; the binary must not
// be stripped, a split-off debug file can be given to load()), demangled with the C++ ABI; other loaded modules by their exported names
// (dladdr). Stacks: libgcc's unwinder over .eh_frame, which every x86-64 binary built by clang or gcc carries, lifted code included, so
// no frame pointers are needed. Inside a fault handler the unwinder walks through the signal trampoline into the interrupted code.
#include "nxrt/os/symbols.h"

#include "unwind.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "nxrt/os/mem.h"

namespace nxrt::os::symbols {
namespace {

constexpr uint64_t kMaxSymbolDistance = 1ull << 20;

// ---- the loaded modules ---------------------------------------------------------------------------------------------------------

struct Mod {
    std::string name;
    uint64_t bias = 0;      // dlpi_addr: added to a link-time address
    uint64_t lo = ~0ull;    // lowest and end of the mapped segments (runtime addresses)
    uint64_t hi = 0;
};

int collect_cb(dl_phdr_info* info, size_t, void* arg) {
    auto* v = static_cast<std::vector<Mod>*>(arg);
    Mod m;
    const char* n = info->dlpi_name ? info->dlpi_name : "";
    m.name = n;
    m.bias = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& p = info->dlpi_phdr[i];
        if (p.p_type != PT_LOAD) continue;
        m.lo = std::min<uint64_t>(m.lo, info->dlpi_addr + (p.p_vaddr & ~0xfffull));
        m.hi = std::max<uint64_t>(m.hi, info->dlpi_addr + p.p_vaddr + p.p_memsz);
    }
    if (m.lo != ~0ull) v->push_back(std::move(m));
    return 0;
}

std::vector<Mod> loaded_modules() {
    std::vector<Mod> v;
    dl_iterate_phdr(collect_cb, &v);
    return v;
}

std::string base_name(const std::string& p) {
    const size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string exe_file_name() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "?";
    buf[n] = 0;
    std::string s = base_name(buf);
    const std::string deleted = " (deleted)";
    if (s.size() > deleted.size() && s.compare(s.size() - deleted.size(), deleted.size(), deleted) == 0) s.resize(s.size() - deleted.size());
    return s;
}

// ---- the symbol table of the main exe -------------------------------------------------------------------------------------------

struct Entry {
    uint64_t value;     // link-time address
    uint64_t size;
    uint32_t name_off;  // into Table::blob
};

struct Table {
    std::vector<Entry> entries;
    std::string blob;   // NUL-terminated names, mangled
};

std::mutex g_mu;
std::shared_ptr<const Table> g_table = std::make_shared<Table>();
bool g_loaded = false;

std::shared_ptr<const Table> current_table() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_table;
}

// Reads the function symbols of the ELF file at `path`. An empty table when the file has none.
std::shared_ptr<Table> read_elf_symbols(const char* path) {
    auto t = std::make_shared<Table>();
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return t;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(ElfW(Ehdr)))) {
        close(fd);
        return t;
    }
    void* map = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return t;
    const auto* base = static_cast<const unsigned char*>(map);
    const size_t fsize = static_cast<size_t>(st.st_size);
    const auto* eh = reinterpret_cast<const ElfW(Ehdr)*>(base);
    do {
        if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64) break;
        if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(ElfW(Shdr)) || eh->e_shoff + static_cast<uint64_t>(eh->e_shnum) * sizeof(ElfW(Shdr)) > fsize) break;
        const auto* sh = reinterpret_cast<const ElfW(Shdr)*>(base + eh->e_shoff);
        const ElfW(Shdr)* symtab = nullptr;
        for (int pass = 0; pass < 2 && !symtab; ++pass)       // .symtab first, the dynamic table when the file is stripped
            for (unsigned i = 0; i < eh->e_shnum; ++i)
                if (sh[i].sh_type == (pass == 0 ? SHT_SYMTAB : SHT_DYNSYM)) {
                    symtab = &sh[i];
                    break;
                }
        if (!symtab || symtab->sh_link >= eh->e_shnum) break;
        const ElfW(Shdr)& str = sh[symtab->sh_link];
        if (symtab->sh_offset + symtab->sh_size > fsize || str.sh_offset + str.sh_size > fsize || symtab->sh_entsize != sizeof(ElfW(Sym))) break;
        const auto* syms = reinterpret_cast<const ElfW(Sym)*>(base + symtab->sh_offset);
        const char* strs = reinterpret_cast<const char*>(base + str.sh_offset);
        const size_t n = symtab->sh_size / sizeof(ElfW(Sym));
        t->entries.reserve(n / 2);
        for (size_t i = 0; i < n; ++i) {
            const ElfW(Sym)& s = syms[i];
            if (ELF64_ST_TYPE(s.st_info) != STT_FUNC || s.st_shndx == SHN_UNDEF || s.st_value == 0 || s.st_name >= str.sh_size) continue;
            const char* name = strs + s.st_name;
            const size_t len = strnlen(name, str.sh_size - s.st_name);
            if (len == 0 || len >= str.sh_size - s.st_name) continue;
            if (t->blob.size() > 0xFFFFFF00u) break;
            t->entries.push_back({s.st_value, s.st_size, static_cast<uint32_t>(t->blob.size())});
            t->blob.append(name, len + 1);
        }
        std::stable_sort(t->entries.begin(), t->entries.end(), [](const Entry& a, const Entry& b) { return a.value < b.value; });
    } while (false);
    munmap(map, fsize);
    return t;
}

std::string demangle(const char* name) {
    if (name[0] == '_' && name[1] == 'Z') {
        int status = 0;
        char* d = abi::__cxa_demangle(name, nullptr, nullptr, &status);
        if (d) {
            std::string s = d;
            std::free(d);
            return s;
        }
    }
    return name;
}

void ensure_loaded() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_loaded) return;
    }
    load(nullptr);
}

}  // namespace

size_t load(const char* path) {
    std::shared_ptr<Table> t = read_elf_symbols(path && *path ? path : "/proc/self/exe");
    const size_t n = t->entries.size();
    std::lock_guard<std::mutex> lk(g_mu);
    g_table = std::move(t);
    g_loaded = true;
    return n;
}

bool text_tables_supported() { return false; }
size_t load_text(const std::string&) { return 0; }

size_t count() {
    ensure_loaded();
    return current_table()->entries.size();
}

void prepare() { ensure_loaded(); }

std::string main_name() { return exe_file_name(); }

uint64_t main_base() {
    const std::vector<Mod> m = loaded_modules();
    return m.empty() ? 0 : m.front().lo;
}

std::vector<Module> modules() {
    std::vector<Module> out;
    const std::vector<Mod> mods = loaded_modules();
    for (size_t i = 0; i < mods.size(); ++i) {
        Module m;
        m.name = i == 0 ? exe_file_name() : base_name(mods[i].name);
        if (m.name.empty()) m.name = "?";
        m.base = mods[i].lo;
        m.size = mods[i].hi - mods[i].lo;
        out.push_back(std::move(m));
    }
    return out;
}

Info lookup(uint64_t addr) {
    ensure_loaded();
    Info info;
    const std::vector<Mod> mods = loaded_modules();
    const Mod* hit = nullptr;
    for (const Mod& m : mods)
        if (addr >= m.lo && addr < m.hi) {
            hit = &m;
            break;
        }
    if (!hit) {
        info.module = "?";
        info.offset = addr;
        return info;
    }
    const bool is_main = hit == &mods.front();
    info.module = is_main ? exe_file_name() : base_name(hit->name);
    if (info.module.empty()) info.module = "?";
    info.offset = addr - hit->lo;
    if (is_main) {
        const std::shared_ptr<const Table> t = current_table();
        const auto& es = t->entries;
        const uint64_t link = addr - hit->bias;
        auto it = std::upper_bound(es.begin(), es.end(), link, [](uint64_t v, const Entry& e) { return v < e.value; });
        if (it == es.begin()) return info;
        --it;
        const uint64_t dist = link - it->value;
        if (it->size ? dist >= it->size : dist > kMaxSymbolDistance) return info;
        info.symbol = demangle(t->blob.c_str() + it->name_off);
        info.sym_offset = dist;
        info.from_table = true;
        return info;
    }
    Dl_info di;
    if (dladdr(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), &di) && di.dli_sname && di.dli_saddr) {
        const uint64_t at = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(di.dli_saddr));
        if (addr >= at && addr - at <= kMaxSymbolDistance) {
            info.symbol = demangle(di.dli_sname);
            info.sym_offset = addr - at;
        }
    }
    return info;
}

// ---- stacks -------------------------------------------------------------------------------------------------------------------------

struct Walk {
    uint64_t* frames;
    size_t max, n, skip;
    uint64_t match;
    bool started;
    bool use_match;
};

_Unwind_Reason_Code walk_cb(_Unwind_Context* ctx, void* arg) {
    auto* w = static_cast<Walk*>(arg);
    const uint64_t ip = static_cast<uint64_t>(_Unwind_GetIP(ctx));
    if (!w->started) {
        if (w->use_match) {
            if (ip != w->match) return _URC_NO_REASON;
        } else if (w->skip > 0) {
            --w->skip;
            return _URC_NO_REASON;
        }
        w->started = true;
    }
    if (w->n >= w->max || (ip == 0 && w->n > 0)) return _URC_END_OF_STACK;   // the outermost frame of a thread has a return address of 0
    w->frames[w->n++] = ip;
    return w->n >= w->max ? _URC_END_OF_STACK : _URC_NO_REASON;
}

}  // namespace nxrt::os::symbols

namespace nxrt::os::detail {

size_t unwind_from_handler(uint64_t match_pc, uint64_t* frames, size_t max) {
    symbols::Walk w{frames, max, 0, 0, match_pc, false, true};
    _Unwind_Backtrace(symbols::walk_cb, &w);
    return w.n;
}

}  // namespace nxrt::os::detail

namespace nxrt::os::symbols {

__attribute__((noinline)) size_t capture_current(uint64_t* frames, size_t max, size_t skip) {
    Walk w{frames, max, 0, skip + 1, 0, false, false};   // + 1: this function's own frame
    _Unwind_Backtrace(walk_cb, &w);
    return w.n;
}

namespace {

bool in_any_module(uint64_t pc) {
    for (const Mod& m : loaded_modules())
        if (pc >= m.lo && pc < m.hi) return true;
    return false;
}

}  // namespace

size_t capture_fault(const FaultInfo& fi, uint64_t* frames, size_t max) {
    if (max == 0) return 0;
    const auto* uc = static_cast<const ucontext_t*>(fi.native);
    // GuestTrap: no machine context; fi.pc is the return address into the code that raised the trap (posix/fault.cpp), found in the unwind.
    const uint64_t pc = fi.pc;
    Walk w{frames, max, 0, 0, pc, false, true};
    _Unwind_Backtrace(walk_cb, &w);
    if (w.n > 0 || !uc) return w.n;
    // The unwinder could not walk (a jump to an address with no unwind data, such as a null function pointer): frame 0, then the return address
    // the call left on the stack, then the frame-pointer chain of the code that made the call.
    size_t n = 0;
    frames[n++] = pc;
    uint64_t sp = fi.sp;
    uint64_t bp = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RBP]);
    if (!in_any_module(pc) && n < max && sp && mem::is_accessible(sp, 8)) frames[n++] = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(sp));
    while (n < max && bp > sp && (bp & 7) == 0 && mem::is_accessible(bp, 16)) {
        const uint64_t ret = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(bp + 8));
        const uint64_t next = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(bp));
        if (!ret) break;
        frames[n++] = ret;
        if (next <= bp) break;
        bp = next;
    }
    return n;
}

}  // namespace nxrt::os::symbols
