// SPDX-License-Identifier: GPL-3.0-or-later
// Guest address space on the host (layout and contract: include/nxrt/memory.h).
//  - fixed reservations at exact addresses (images, stacks, the kernel region), committed on demand;
//  - the heap: one pagefile-backed section (SEC_RESERVE) with a primary view at kHeapBase and alias views of the same
//    section pages, so one physical page can be seen at two guest addresses;
//  - a Horizon-style vm map kept beside the real mappings for svcQueryMemory.
#include "nxrt/memory.h"

#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/write_watch.h"

#include "nxrt/os/mem.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace nxrt::mem {
namespace {

constexpr const char* kTag = "mem";
constexpr uint64_t kAddrLimit = 0x800000000000ull;  // user-mode address limit of x64 Windows (47 bits)
constexpr uint64_t kVmEnd = 0x8000000000ull;
constexpr int kAliasTries = 16;

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

bool page_span(uint64_t addr, uint64_t size, uint64_t* start, uint64_t* end) {
    if (size > kAddrLimit || addr > kAddrLimit - size) return false;
    *start = addr & ~(kPage - 1);
    *end = align_up(addr + size, kPage);
    return true;
}

// ---- fixed reservations (guarded by g_fixed_mu) ----
struct Fixed {
    uint64_t size;
    std::string tag;
};
std::mutex g_fixed_mu;
std::map<uint64_t, Fixed> g_fixed;

// ---- module state ----
std::mutex g_state_mu;
std::atomic<bool> g_inited{false};
std::atomic<uint64_t> g_default_heap{0};

std::mutex g_kernel_mu;
uint64_t g_kernel_next = kKernelBase;     // guarded by g_kernel_mu

std::mutex g_vm_mu;
std::map<uint64_t, VmInfo> g_vm;          // start -> block, guarded by g_vm_mu

// ---- heap (guarded by g_heap_mu) ----
std::mutex g_heap_mu;
os::mem::Section g_section;
uint64_t g_heap_size = 0;
std::vector<uint64_t> g_commit_bits;
uint64_t g_committed = 0;
// Per 4 KiB page of the section, what the host reports for it: 0 never committed, 1 committed but no-access (a decommitted page or one
// the guest made inaccessible), 2 committed and accessible. VirtualQuery over the section view walks every page table entry of the
// run of identical pages (about 75 ms for a 3 GiB committed heap), so is_mapped and protect answer heap ranges from this table.
std::vector<uint8_t> g_heap_cls;
struct Alias {
    uint64_t size;
    uint64_t offset;
};
std::map<uint64_t, Alias> g_aliases;
std::map<uint64_t, uint64_t> g_alias_used;
std::map<uint64_t, uint64_t> g_private_views;   // address -> size of the views heap_private_view handed out

// ---- gauges (include/nxrt/gauge.h); each callback takes the lock of the state it reads ----
// Removes its gauge when this file's statics are torn down (declared after them, so it goes first).
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};
GaugeHook gauge_heap_reserved("mem.heap_reserved", [] { return static_cast<int64_t>(heap_reserved()); });
GaugeHook gauge_heap_committed("mem.heap_committed", [] { return static_cast<int64_t>(heap_committed()); });
GaugeHook gauge_aliases("mem.aliases", [] {
    std::lock_guard<std::mutex> lk(g_heap_mu);
    return static_cast<int64_t>(g_aliases.size());
});
GaugeHook gauge_alias_used("mem.alias_used", [] {
    std::lock_guard<std::mutex> lk(g_heap_mu);
    return static_cast<int64_t>(g_alias_used.size());
});
GaugeHook gauge_fixed("mem.fixed_regions", [] {
    std::lock_guard<std::mutex> lk(g_fixed_mu);
    return static_cast<int64_t>(g_fixed.size());
});
GaugeHook gauge_fixed_bytes("mem.fixed_bytes", [] {
    std::lock_guard<std::mutex> lk(g_fixed_mu);
    uint64_t n = 0;
    for (const auto& kv : g_fixed) n += kv.second.size;
    return static_cast<int64_t>(n);
});
GaugeHook gauge_vm_blocks("mem.vm_blocks", [] {
    std::lock_guard<std::mutex> lk(g_vm_mu);
    return static_cast<int64_t>(g_vm.size());
});

// [memory] fast_query = false: ask the host (VirtualQuery) instead of the page table above; for before/after measurements.
bool fast_query() {
    static const bool v = config_get_bool("memory", "fast_query", true);
    return v;
}

bool in_heap_view(uint64_t start, uint64_t end) {
    return fast_query() && g_heap_size && !g_heap_cls.empty() && start >= kHeapBase && end <= kHeapBase + g_heap_size && start < end;
}

bool bit_test(uint64_t page) { return (g_commit_bits[page / 64] >> (page % 64)) & 1; }
void bit_set(uint64_t page) { g_commit_bits[page / 64] |= uint64_t{1} << (page % 64); }
void bit_clear(uint64_t page) { g_commit_bits[page / 64] &= ~(uint64_t{1} << (page % 64)); }

uint64_t find_free_alias(uint64_t from, uint64_t size) {
    const uint64_t limit = kAliasBase + kAliasSize;
    uint64_t cand = std::max(from, kAliasBase);
    for (;;) {
        if (cand >= limit || size > limit - cand) return 0;
        bool moved = false;
        auto it = g_alias_used.upper_bound(cand);
        if (it != g_alias_used.begin()) {
            auto p = std::prev(it);
            if (p->first + p->second > cand) {
                cand = align_up(p->first + p->second, kAllocGranularity);
                moved = true;
            }
        }
        if (!moved && it != g_alias_used.end() && it->first < cand + size) {
            cand = align_up(it->first + it->second, kAllocGranularity);
            moved = true;
        }
        if (!moved) return cand;
    }
}

}

// ---------------------------------------------------------------------------------------------------------------------
// init / shutdown

bool init(const Config& cfg) {
    std::lock_guard lk(g_state_mu);
    if (g_inited.load()) return true;

    uint64_t heap = cfg.heap_reserve;
    if (heap == 0) {
        int64_t gb = nxrt::config_get_int("memory", "heap_gb", 6);
        if (gb < 1 || gb > 1024) {
            NXLOG_W(kTag, "[memory] heap_gb=%lld is out of range: using 6", static_cast<long long>(gb));
            gb = 6;
        }
        heap = static_cast<uint64_t>(gb) << 30;
    }
    g_default_heap.store(align_up(heap, kAllocGranularity));

    if (cfg.reserve_regions) {
        if (!reserve_fixed(kStackBase, kStackSize, "stack")) {
            NXLOG_E(kTag, "init failed: the stack region could not be reserved");
            return false;
        }
        if (!reserve_fixed(kKernelBase, kKernelSize, "kernel")) {
            NXLOG_E(kTag, "init failed: the kernel region could not be reserved");
            release_fixed(kStackBase);
            return false;
        }
    }
    {
        std::lock_guard kl(g_kernel_mu);
        g_kernel_next = kKernelBase;
    }
    g_inited.store(true);
    NXLOG_I(kTag, "init: stack %llx (+%llx), kernel %llx (+%llx), default heap %llu MiB, regions %s",
            static_cast<unsigned long long>(kStackBase), static_cast<unsigned long long>(kStackSize),
            static_cast<unsigned long long>(kKernelBase), static_cast<unsigned long long>(kKernelSize),
            static_cast<unsigned long long>(g_default_heap.load() >> 20), cfg.reserve_regions ? "reserved" : "skipped");
    return true;
}

void shutdown() {
    std::lock_guard lk(g_state_mu);
    {
        std::lock_guard hl(g_heap_mu);
        for (auto& [view, rec] : g_aliases) {
            if (!os::mem::section_unmap(view, rec.size)) {
                NXLOG_E(kTag, "shutdown: unmapping alias %llx failed", static_cast<unsigned long long>(view));
            }
        }
        g_aliases.clear();
        g_alias_used.clear();
        if (g_heap_size) {
            if (!os::mem::section_unmap(kHeapBase, g_heap_size)) NXLOG_E(kTag, "shutdown: unmapping the heap view failed");
        }
        for (auto& [view, size] : g_private_views) os::mem::section_unmap(view, size);
        g_private_views.clear();
        os::mem::section_close(&g_section);
        g_heap_size = 0;
        g_commit_bits.clear();
        g_heap_cls.clear();
        g_committed = 0;
    }
    {
        std::lock_guard fl(g_fixed_mu);
        for (auto& [addr, f] : g_fixed) {
            if (!os::mem::release(addr, f.size)) NXLOG_E(kTag, "shutdown: releasing %llx failed", static_cast<unsigned long long>(addr));
        }
        g_fixed.clear();
    }
    {
        std::lock_guard vl(g_vm_mu);
        g_vm.clear();
    }
    {
        std::lock_guard kl(g_kernel_mu);
        g_kernel_next = kKernelBase;
    }
    g_default_heap.store(0);
    g_inited.store(false);
    NXLOG_I(kTag, "shutdown: all mappings of this module released");
}

bool initialised() { return g_inited.load(); }

// ---------------------------------------------------------------------------------------------------------------------
// fixed mappings

bool reserve_fixed(uint64_t addr, uint64_t size, const char* tag) {
    if (size == 0 || addr % kAllocGranularity != 0 || addr >= kAddrLimit || size > kAddrLimit - addr) {
        NXLOG_E(kTag, "reserve %llx (+%llx) %s: address must be 64 KiB aligned and inside the user address space",
                static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size), tag ? tag : "");
        return false;
    }
    size = align_up(size, kPage);
    std::lock_guard lk(g_fixed_mu);
    if (!os::mem::reserve_fixed(addr, size)) {
        NXLOG_E(kTag, "reserve %llx (+%llx) %s: the range is not free or the host refused it",
                static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size), tag ? tag : "");
        return false;
    }
    g_fixed[addr] = Fixed{size, tag ? tag : ""};
    NXLOG_D(kTag, "reserved %llx (+%llx) %s", static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(size), tag ? tag : "");
    return true;
}

bool release_fixed(uint64_t addr) {
    std::lock_guard lk(g_fixed_mu);
    auto it = g_fixed.find(addr);
    if (it == g_fixed.end()) {
        NXLOG_W(kTag, "release %llx: no reservation of this module starts there", static_cast<unsigned long long>(addr));
        return false;
    }
    if (!os::mem::release(addr, it->second.size)) {
        NXLOG_E(kTag, "release %llx: the host refused it", static_cast<unsigned long long>(addr));
        return false;
    }
    g_fixed.erase(it);
    return true;
}

bool commit(uint64_t addr, uint64_t size, Prot p) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return false;
    if (start == end) return true;
    const ww::OsWriteScope wws(reinterpret_cast<void*>(start), end - start);
    if (!os::mem::commit(start, end - start, p)) {
        NXLOG_E(kTag, "commit %llx (+%llx): the host refused it", static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end - start));
        return false;
    }
    return true;
}

bool decommit(uint64_t addr, uint64_t size) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return false;
    if (start == end) return true;
    const ww::OsWriteScope wws(reinterpret_cast<void*>(start), end - start);
    if (!os::mem::decommit(start, end - start)) {
        NXLOG_E(kTag, "decommit %llx (+%llx): the host refused it", static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end - start));
        return false;
    }
    return true;
}

bool protect(uint64_t addr, uint64_t size, Prot p) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return false;
    if (start == end) return true;
    const ww::OsWriteScope wws(reinterpret_cast<void*>(start), end - start);   // pages the write tracker armed get this protection, not its own
    {   // heap: the page table says whether every page is committed, so one protect call does it (no query over the view)
        std::lock_guard hk(g_heap_mu);
        if (in_heap_view(start, end)) {
            const uint64_t p0 = (start - kHeapBase) / kPage, p1 = (end - kHeapBase) / kPage;
            bool all = true;
            for (uint64_t q = p0; q < p1 && all; ++q) all = g_heap_cls[q] != 0;
            if (all && os::mem::protect(start, end - start, p, false)) {
                std::fill(g_heap_cls.begin() + static_cast<std::ptrdiff_t>(p0), g_heap_cls.begin() + static_cast<std::ptrdiff_t>(p1), p == Prot::None ? uint8_t{1} : uint8_t{2});
                return true;
            }
        }
    }
    if (!os::mem::protect(start, end - start, p)) {
        NXLOG_E(kTag, "protect %llx (+%llx): a page is not committed or the host refused it", static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end - start));
        return false;
    }
    return true;
}

uint64_t alloc_kernel(uint64_t size, uint64_t align) {
    if (!initialised() && !init()) return 0;
    if (align == 0) align = kPage;
    if (align & (align - 1)) {
        NXLOG_E(kTag, "alloc_kernel: alignment %llx is not a power of two", static_cast<unsigned long long>(align));
        return 0;
    }
    align = std::max(align, kPage);
    if (size == 0) size = 1;
    if (size > kKernelSize) return 0;
    size = align_up(size, kPage);

    std::lock_guard lk(g_kernel_mu);
    const uint64_t a = align_up(g_kernel_next, align);
    if (a < g_kernel_next || a > kKernelBase + kKernelSize || size > kKernelBase + kKernelSize - a) {
        NXLOG_E(kTag, "alloc_kernel: kernel region exhausted (%llx used of %llx)",
                static_cast<unsigned long long>(g_kernel_next - kKernelBase),
                static_cast<unsigned long long>(kKernelSize));
        return 0;
    }
    if (!nxrt::mem::commit(a, size, Prot::RW)) return 0;
    g_kernel_next = a + size;
    return a;
}

bool is_mapped(uint64_t addr, uint64_t size) {
    uint64_t start = 0, end = 0;
    if (!page_span(addr, size, &start, &end)) return false;
    {
        std::lock_guard hk(g_heap_mu);
        if (in_heap_view(start, end)) {
            const uint64_t p0 = (start - kHeapBase) / kPage, p1 = (end - kHeapBase) / kPage;
            for (uint64_t q = p0; q < p1; ++q)
                if (g_heap_cls[q] != 2) return false;
            return true;
        }
    }
    return os::mem::is_accessible(start, end - start);
}

// ---------------------------------------------------------------------------------------------------------------------
// heap section

bool heap_create(uint64_t reserve_bytes) {
    if (!initialised() && !init()) return false;
    std::lock_guard lk(g_heap_mu);
    if (g_heap_size) return true;

    const uint64_t want = reserve_bytes ? reserve_bytes : g_default_heap.load();
    if (want == 0) {
        NXLOG_E(kTag, "heap_create: no size given and no configured default");
        return false;
    }
    const uint64_t size = align_up(want, kAllocGranularity);
    if (size > kHeapMax) {
        NXLOG_E(kTag, "heap_create: %llu bytes exceed the heap region limit of %llu bytes",
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(kHeapMax));
        return false;
    }
    os::mem::Section sec;
    if (!os::mem::section_create(size, &sec)) {
        NXLOG_E(kTag, "heap_create: creating the section of %llu MiB failed", static_cast<unsigned long long>(size >> 20));
        return false;
    }
    // Prot::None: the view's pages are reserved until heap_commit makes them usable.
    if (os::mem::section_map(sec, 0, size, kHeapBase, Prot::None) != kHeapBase) {
        NXLOG_E(kTag, "heap_create: mapping the primary view at %llx failed", static_cast<unsigned long long>(kHeapBase));
        os::mem::section_close(&sec);
        return false;
    }
    g_section = sec;
    g_heap_size = size;
    g_commit_bits.assign((size / kPage + 63) / 64, 0);
    g_heap_cls.assign(static_cast<size_t>(size / kPage), 0);
    g_committed = 0;
    NXLOG_I(kTag, "heap: section of %llu MiB, primary view at %llx", static_cast<unsigned long long>(size >> 20),
            static_cast<unsigned long long>(kHeapBase));
    return true;
}

bool heap_exists() {
    std::lock_guard lk(g_heap_mu);
    return g_heap_size != 0;
}

uint64_t heap_reserved() {
    std::lock_guard lk(g_heap_mu);
    return g_heap_size;
}

bool heap_commit(uint64_t offset, uint64_t size) {
    std::lock_guard lk(g_heap_mu);
    if (!g_heap_size) {
        NXLOG_W(kTag, "heap_commit: no heap");
        return false;
    }
    if (size == 0) return true;
    if (offset > g_heap_size || size > g_heap_size - offset) {
        NXLOG_E(kTag, "heap_commit: [%llx, +%llx) is outside the %llu MiB section",
                static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(g_heap_size >> 20));
        return false;
    }
    const uint64_t start = offset & ~(kPage - 1);
    const uint64_t end = align_up(offset + size, kPage);
    const ww::OsWriteScope wws(reinterpret_cast<void*>(kHeapBase + start), end - start);   // MEM_COMMIT makes committed pages read-write again
    if (!os::mem::commit(kHeapBase + start, end - start, Prot::RW)) {
        NXLOG_E(kTag, "heap_commit [%llx, +%llx): the host refused it", static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end - start));
        return false;
    }
    for (uint64_t p = start / kPage; p < end / kPage; p++) {
        if (!bit_test(p)) {
            bit_set(p);
            g_committed += kPage;
        }
        g_heap_cls[p] = 2;       // MEM_COMMIT with PAGE_READWRITE applies to pages that were committed already too
    }
    return true;
}

bool heap_decommit(uint64_t offset, uint64_t size) {
    std::lock_guard lk(g_heap_mu);
    if (!g_heap_size) return false;
    if (size == 0) return true;
    if (offset > g_heap_size || size > g_heap_size - offset) {
        NXLOG_E(kTag, "heap_decommit: [%llx, +%llx) is outside the section", static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(size));
        return false;
    }
    // Windows refuses MEM_DECOMMIT on pages of a section view, so the pages are made inaccessible (Prot::None) through the primary view
    // instead (Linux could free them, but the two systems behave alike here): guest accesses fault, the host pages stay charged to the section
    // (as in smo-recomp's SetMemoryHeapSize), and the alias views still show the section's contents.
    const uint64_t start = offset & ~(kPage - 1);
    const uint64_t end = align_up(offset + size, kPage);
    const ww::OsWriteScope wws(reinterpret_cast<void*>(kHeapBase + start), end - start);
    for (uint64_t p = start / kPage; p < end / kPage;) {
        if (!bit_test(p)) {
            p++;
            continue;
        }
        uint64_t run = p;
        while (run < end / kPage && bit_test(run)) run++;
        if (!os::mem::protect(kHeapBase + p * kPage, (run - p) * kPage, Prot::None, false)) {
            NXLOG_E(kTag, "heap_decommit [%llx, +%llx): the host refused it", static_cast<unsigned long long>(p * kPage),
                    static_cast<unsigned long long>((run - p) * kPage));
            return false;
        }
        for (uint64_t q = p; q < run; q++) {
            bit_clear(q);
            g_committed -= kPage;
            g_heap_cls[q] = 1;
        }
        p = run;
    }
    return true;
}

uint64_t heap_committed() {
    std::lock_guard lk(g_heap_mu);
    return g_committed;
}

uint64_t heap_alias(uint64_t offset, uint64_t size, uint64_t at, Prot p) {
    std::lock_guard lk(g_heap_mu);
    if (!g_heap_size) {
        NXLOG_W(kTag, "heap_alias: no heap");
        return 0;
    }
    if (size == 0 || offset % kAllocGranularity || size % kAllocGranularity || offset > g_heap_size ||
        size > g_heap_size - offset) {
        NXLOG_W(kTag, "heap_alias: [%llx, +%llx) must be 64 KiB multiples inside the section",
                static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size));
        return 0;
    }
    if (at % kAllocGranularity) {
        NXLOG_W(kTag, "heap_alias: target %llx is not 64 KiB aligned", static_cast<unsigned long long>(at));
        return 0;
    }
    ww::ban_section(offset, size);   // before the view exists: writes through it would not fault in the primary view
    if (at != 0) {
        if (os::mem::section_map(g_section, offset, size, at, p) != at) {
            NXLOG_E(kTag, "heap_alias at %llx (section %llx, +%llx): mapping the view failed (range taken?)",
                    static_cast<unsigned long long>(at), static_cast<unsigned long long>(offset),
                    static_cast<unsigned long long>(size));
            return 0;
        }
        g_aliases.emplace(at, Alias{size, offset});
        g_alias_used[at] = size;
        NXLOG_I(kTag, "heap alias: section %llx (+%llx) at %llx", static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(at));
        return at;
    }

    uint64_t cand = kAliasBase;
    for (int tries = 0; tries < kAliasTries; tries++) {
        cand = find_free_alias(cand, size);
        if (!cand) break;
        if (os::mem::section_map(g_section, offset, size, cand, p) == cand) {
            g_aliases.emplace(cand, Alias{size, offset});
            g_alias_used[cand] = size;
            NXLOG_I(kTag, "heap alias: section %llx (+%llx) at %llx", static_cast<unsigned long long>(offset),
                    static_cast<unsigned long long>(size), static_cast<unsigned long long>(cand));
            return cand;
        }
        NXLOG_W(kTag, "heap_alias: %llx is taken: marking it unusable", static_cast<unsigned long long>(cand));
        g_alias_used[cand] = size;
    }
    NXLOG_E(kTag, "heap_alias: no free range for %llx bytes in the alias region", static_cast<unsigned long long>(size));
    return 0;
}

void* heap_private_view(uint64_t offset, uint64_t size) {
    std::lock_guard lk(g_heap_mu);
    if (!g_heap_size || size == 0 || offset % kAllocGranularity || offset > g_heap_size || size > g_heap_size - offset) return nullptr;
    const uint64_t v = os::mem::section_map(g_section, offset, size, 0, Prot::RW);
    if (v) g_private_views[v] = size;
    return reinterpret_cast<void*>(v);
}

bool heap_private_view_unmap(void* view) {
    std::lock_guard lk(g_heap_mu);
    auto it = g_private_views.find(reinterpret_cast<uint64_t>(view));
    if (it == g_private_views.end()) return false;
    const bool ok = os::mem::section_unmap(it->first, it->second);
    g_private_views.erase(it);
    return ok;
}

bool heap_alias_unmap(uint64_t view_addr) {
    std::lock_guard lk(g_heap_mu);
    auto it = g_aliases.find(view_addr);
    if (it == g_aliases.end()) {
        NXLOG_W(kTag, "heap_alias_unmap: %llx is not an alias view", static_cast<unsigned long long>(view_addr));
        return false;
    }
    if (!os::mem::section_unmap(view_addr, it->second.size)) {
        NXLOG_E(kTag, "heap_alias_unmap %llx: unmapping the view failed", static_cast<unsigned long long>(view_addr));
        return false;
    }
    g_alias_used.erase(view_addr);
    g_aliases.erase(it);
    return true;
}

void heap_alias_ranges(std::vector<std::pair<uint64_t, uint64_t>>& out) {
    std::lock_guard lk(g_heap_mu);
    for (const auto& a : g_aliases) out.emplace_back(a.second.offset, a.second.size);
}

size_t heap_alias_count() {
    std::lock_guard lk(g_heap_mu);
    return g_aliases.size();
}

bool heap_offset_of(uint64_t addr, uint64_t* offset) {
    if (!offset) return false;
    std::lock_guard lk(g_heap_mu);
    if (g_heap_size && addr >= kHeapBase && addr - kHeapBase < g_heap_size) {
        *offset = addr - kHeapBase;
        return true;
    }
    auto it = g_aliases.upper_bound(addr);
    if (it == g_aliases.begin()) return false;
    --it;
    if (addr - it->first < it->second.size) {
        *offset = it->second.offset + (addr - it->first);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// vm map (svcQueryMemory)

void vm_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm) {
    if (size == 0 || addr + size < addr) return;
    const uint64_t end = addr + size;
    std::lock_guard lk(g_vm_mu);
    auto it = g_vm.lower_bound(addr);
    if (it != g_vm.begin()) {
        auto p = std::prev(it);
        if (p->second.addr + p->second.size > addr) it = p;
    }
    while (it != g_vm.end() && it->second.addr < end) {
        VmInfo b = it->second;
        it = g_vm.erase(it);
        if (b.addr < addr) g_vm.emplace(b.addr, VmInfo{b.addr, addr - b.addr, b.state, b.perm});
        if (b.addr + b.size > end) g_vm.emplace(end, VmInfo{end, b.addr + b.size - end, b.state, b.perm});
    }
    if (state != kMemFree) g_vm.emplace(addr, VmInfo{addr, size, state, perm});
}

VmInfo vm_query(uint64_t addr) {
    std::lock_guard lk(g_vm_mu);
    auto it = g_vm.upper_bound(addr);
    if (it != g_vm.begin()) {
        auto p = std::prev(it);
        if (addr < p->second.addr + p->second.size) return p->second;
        const uint64_t gap_start = p->second.addr + p->second.size;
        const uint64_t gap_end = it == g_vm.end() ? kVmEnd : it->first;
        return VmInfo{gap_start, gap_end - gap_start, kMemFree, 0};
    }
    const uint64_t gap_end = it == g_vm.end() ? kVmEnd : it->first;
    return VmInfo{0, gap_end, kMemFree, 0};
}

}
