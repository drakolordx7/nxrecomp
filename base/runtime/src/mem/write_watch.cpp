// SPDX-License-Identifier: GPL-3.0-or-later
// Page write tracking by page protection (include/nxrt/write_watch.h). Four bitmaps per region, one bit per 4 KiB page:
//   prot    the page is read-only because of us (set by arm, cleared by whoever makes it writable again)
//   valid   armed and not written since (the owner's copy of the page is current)
//   written the CPU wrote it since the owner last asked (take_written)
//   ban     never arm (another view of the page exists)
// The fault handler and before_os_write may run on any thread at any time (on Linux the handler is a signal handler); they only use atomics,
// os::mem::protect_if and the region's spin reader/writer lock (RwLock below; a blocking lock would not be legal in a signal handler): whoever makes pages writable holds it shared, arm() and region_destroy() hold it exclusive, so a page's bit
// and its protection change together as seen by everybody else (no page ends up read-only without its bit, or armed and writable). Regions are
// published in a fixed table; a reader counts itself in g_busy while it looks at a region, and region_destroy waits for that count to
// drop after taking the region out of the table.
#include "nxrt/write_watch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/os/fault.h"
#include "nxrt/os/mem.h"

namespace nxrt::mem::ww {

// A reader/writer lock made of atomics (usable inside a fault handler on both systems): shared = reader count, exclusive = -1; a waiting writer
// keeps new readers out. Nobody takes it twice on one thread.
struct RwLock {
    std::atomic<int> state{0};
    std::atomic<int> writers{0};
    static void backoff(int i) {
        if (i < 64) {
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#endif
        } else if (i < 2000) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    void lock_shared() {
        for (int i = 0;; ++i) {
            if (writers.load(std::memory_order_acquire) == 0) {
                int st = state.load(std::memory_order_acquire);
                if (st >= 0 && state.compare_exchange_weak(st, st + 1, std::memory_order_acq_rel)) return;
            }
            backoff(i);
        }
    }
    void unlock_shared() { state.fetch_sub(1, std::memory_order_release); }
    void lock() {
        writers.fetch_add(1, std::memory_order_acq_rel);
        for (int i = 0;; ++i) {
            int st = 0;
            if (state.compare_exchange_weak(st, -1, std::memory_order_acq_rel)) return;
            backoff(i);
        }
    }
    void unlock() {
        state.store(0, std::memory_order_release);
        writers.fetch_sub(1, std::memory_order_acq_rel);
    }
};

struct Region {
    uint64_t base = 0;
    uint64_t npages = 0;
    uint64_t nwords = 0;
    std::unique_ptr<std::atomic<uint64_t>[]> prot, valid, written, ban;
    int slot = -1;
    RwLock lock;
    std::unique_ptr<std::atomic<uint16_t>[]> releasing;
};

struct SharedLock {
    explicit SharedLock(Region* r) : r_(r) { r_->lock.lock_shared(); }
    ~SharedLock() { r_->lock.unlock_shared(); }
    Region* r_;
};
struct ExclusiveLock {
    explicit ExclusiveLock(Region* r) : r_(r) { r_->lock.lock(); }
    ~ExclusiveLock() { r_->lock.unlock(); }
    Region* r_;
};

namespace {

constexpr const char* kTag = "mem.ww";
constexpr int kSlots = 512;
std::atomic<Region*> g_slots[kSlots];
std::atomic<int> g_hi{0};
std::atomic<int> g_busy{0};
std::atomic<int> g_live{0};
std::atomic<uint64_t> g_faults{0};
std::atomic<uint64_t> g_waits{0};
std::mutex g_mu;
int g_fault_id = -1;

// Operating-system writes in progress (OsWriteScope): arm() leaves these ranges alone, so a page cannot become read-only between the
// release in before_os_write and the end of the ReadFile that fills it. When every slot is taken, g_os_overflow stops all arming.
constexpr int kOsSlots = 32;
struct OsRange { std::atomic<uint64_t> lo{0}, hi{0}; };
OsRange g_os[kOsSlots];
std::atomic<int> g_os_overflow{0};

bool os_write_overlaps(uint64_t lo, uint64_t hi) {
    if (g_os_overflow.load(std::memory_order_seq_cst)) return true;
    for (OsRange& o : g_os) {
        const uint64_t h = o.hi.load(std::memory_order_seq_cst);
        if (h && o.lo.load(std::memory_order_seq_cst) < hi && lo < h) return true;
    }
    return false;
}

uint64_t bit(uint64_t page) { return uint64_t{1} << (page & 63); }

bool test(const std::atomic<uint64_t>* bm, uint64_t page) { return (bm[page >> 6].load(std::memory_order_acquire) & bit(page)) != 0; }

void set_bits(std::atomic<uint64_t>* bm, uint64_t first, uint64_t count, bool on) {
    uint64_t p = first;
    const uint64_t end = first + count;
    while (p < end) {
        const uint64_t w = p >> 6;
        const uint64_t lo = p & 63;
        const uint64_t hi = std::min<uint64_t>(64, lo + (end - p));
        const uint64_t mask = (hi - lo == 64) ? ~uint64_t{0} : (((uint64_t{1} << (hi - lo)) - 1) << lo);
        if (on) bm[w].fetch_or(mask, std::memory_order_acq_rel);
        else bm[w].fetch_and(~mask, std::memory_order_acq_rel);
        p += hi - lo;
    }
}

bool all_set(const std::atomic<uint64_t>* bm, uint64_t first, uint64_t count) {
    uint64_t p = first;
    const uint64_t end = first + count;
    while (p < end) {
        const uint64_t w = p >> 6;
        const uint64_t lo = p & 63;
        const uint64_t hi = std::min<uint64_t>(64, lo + (end - p));
        const uint64_t mask = (hi - lo == 64) ? ~uint64_t{0} : (((uint64_t{1} << (hi - lo)) - 1) << lo);
        if ((bm[w].load(std::memory_order_acquire) & mask) != mask) return false;
        p += hi - lo;
    }
    return true;
}

bool any_set(const std::atomic<uint64_t>* bm, uint64_t first, uint64_t count) {
    uint64_t p = first;
    const uint64_t end = first + count;
    while (p < end) {
        const uint64_t w = p >> 6;
        const uint64_t lo = p & 63;
        const uint64_t hi = std::min<uint64_t>(64, lo + (end - p));
        const uint64_t mask = (hi - lo == 64) ? ~uint64_t{0} : (((uint64_t{1} << (hi - lo)) - 1) << lo);
        if (bm[w].load(std::memory_order_acquire) & mask) return true;
        p += hi - lo;
    }
    return false;
}

// The protection of the page at addr as the host reports it; false when it is not committed.
bool page_protect(uint64_t addr, os::mem::Prot* out) {
    os::mem::Region r;
    if (!os::mem::query(addr, &r, kPage) || r.state != os::mem::State::Committed) return false;
    *out = r.prot;
    return true;
}

// ---- host mapping limit (Linux vm.max_map_count; the budget is infinite where there is none) ----
// Every armed run is a mapping of its own, and every page released inside one splits it: up to two more mappings each. g_runs estimates
// the number of armed runs; arm() refuses to go over the budget (the owner then treats the pages as changed: it re-reads them). If a page
// cannot be released because the host has run out of mappings anyway, emergency_release() makes all armed pages of the region writable and
// the tracker stops arming (g_failed), the same "everything is dirty" answer.
int64_t run_budget() {
    static const int64_t v = [] {
        const uint64_t mm = os::mem::max_mappings();
        if (!mm) return INT64_MAX;
        return std::max<int64_t>(256, (static_cast<int64_t>(mm) - 12000) / 4);
    }();
    return v;
}
std::atomic<int64_t> g_runs{0};
std::atomic<bool> g_failed{false};

// Armed runs (starts of runs of set bits) in bits [lo, hi) of bm, counting a run that begins before lo as starting at lo.
int64_t count_runs(const std::atomic<uint64_t>* bm, uint64_t lo, uint64_t hi) {
    int64_t n = 0;
    bool prev = false;
    for (uint64_t p = lo; p < hi; ++p) {
        const bool cur = (bm[p >> 6].load(std::memory_order_acquire) >> (p & 63)) & 1;
        if (cur && !prev) ++n;
        prev = cur;
    }
    return n;
}

void emergency_release(Region* r);

// One page of a region becomes writable and counts as written. True when the page is ours to handle (the faulting write may retry).
// The caller holds the region's lock shared.
bool release_page(Region* r, uint64_t page) {
    const uint64_t b = bit(page);
    const uint64_t w = page >> 6;
    // Announce the release before the bit goes: whoever finds the bit already clear can then tell "somebody is making this page writable"
    // (the count is up from before the bit went until after the protection change) from "nobody is": a foreign read-only page.
    std::atomic<uint16_t>& rel = r->releasing[page];
    const bool armed = (r->prot[w].load(std::memory_order_acquire) & b) != 0;
    if (armed) rel.fetch_add(1, std::memory_order_seq_cst);
    const uint64_t old = r->prot[w].fetch_and(~b, std::memory_order_acq_rel);
    r->valid[w].fetch_and(~b, std::memory_order_acq_rel);
    r->written[w].fetch_or(b, std::memory_order_acq_rel);
    const uint64_t addr = r->base + page * kPage;
    if (old & b) {
        if (run_budget() != INT64_MAX) {      // the run splits (both neighbours armed) or vanishes (neither is)
            const bool left = page > 0 && test(r->prot.get(), page - 1), right = page + 1 < r->npages && test(r->prot.get(), page + 1);
            g_runs.fetch_add(left && right ? 1 : (!left && !right ? -1 : 0), std::memory_order_relaxed);
        }
        const os::mem::ProtectResult pr = os::mem::protect_if(addr, kPage, os::mem::Prot::R, os::mem::Prot::RW);
        if (pr == os::mem::ProtectResult::NoMem) emergency_release(r);
        const bool ok = pr == os::mem::ProtectResult::Ok || pr == os::mem::ProtectResult::NoMem;
        rel.fetch_sub(1, std::memory_order_seq_cst);
        return ok;
    }
    if (armed) rel.fetch_sub(1, std::memory_order_seq_cst);
    // Not armed (any more): another releaser (also holding the lock shared) may be making it writable right now. Wait for it only while
    // such a release is in progress; with none, a read-only page is somebody else's and the fault goes on at once. The count is read before
    // the protection: a releaser that cleared the bit before our fetch_and has counted itself in before that, so a zero count means every
    // such release has finished and its effect is visible in the protection we read next.
    // The wait has no round limit: a releaser that is counted in will finish (it only has its protection change left), but it may be descheduled
    // for a long time on a busy machine, and giving up early would pass a fault on that is ours (seen as an unhandled access violation).
    for (int i = 0;; ++i) {
        const uint16_t busy = rel.load(std::memory_order_seq_cst);
        os::mem::Prot pr = os::mem::Prot::None;
        const bool committed = page_protect(addr, &pr);
        if (committed && (pr == os::mem::Prot::RW || pr == os::mem::Prot::RWX)) return true;
        if (!committed || pr != os::mem::Prot::R || busy == 0) return false;
        g_waits.fetch_add(1, std::memory_order_relaxed);
        RwLock::backoff(i);
    }
}

// The host cannot split another mapping: make every armed page of this region writable (whole runs, so no split is needed) and stop arming.
// The caller holds the region's lock shared. Pages count as written, nothing stays valid.
void emergency_release(Region* r) {
    g_failed.store(true, std::memory_order_seq_cst);
    for (uint64_t p = 0; p < r->npages;) {
        if (!test(r->prot.get(), p)) { ++p; continue; }
        uint64_t q = p;
        while (q < r->npages && test(r->prot.get(), q)) ++q;
        os::mem::protect(r->base + p * kPage, (q - p) * kPage, os::mem::Prot::RW, false);
        set_bits(r->prot.get(), p, q - p, false);
        set_bits(r->written.get(), p, q - p, true);
        p = q;
    }
    set_bits(r->valid.get(), 0, r->npages, false);
    g_runs.store(0, std::memory_order_relaxed);
}

os::FaultResult on_fault(const os::FaultInfo& fi, void*) {
    if (fi.kind != os::FaultKind::AccessViolation || !fi.is_write) return os::FaultResult::Continue;
    if (g_live.load(std::memory_order_acquire) == 0) return os::FaultResult::Continue;
    const uint64_t addr = fi.addr;
    bool ours = false;
    g_busy.fetch_add(1, std::memory_order_seq_cst);
    const int hi = g_hi.load(std::memory_order_seq_cst);
    for (int i = 0; i < hi; ++i) {
        Region* r = g_slots[i].load(std::memory_order_seq_cst);
        if (!r || addr < r->base || addr >= r->base + r->npages * kPage) continue;
        SharedLock lk(r);
        ours |= release_page(r, (addr - r->base) / kPage);
    }
    g_busy.fetch_sub(1, std::memory_order_seq_cst);
    if (!ours) return os::FaultResult::Continue;
    g_faults.fetch_add(1, std::memory_order_relaxed);
    return os::FaultResult::Handled;
}

template <class Fn>
void for_regions(uint64_t addr, uint64_t size, Fn&& fn) {
    if (g_live.load(std::memory_order_acquire) == 0 || size == 0) return;
    g_busy.fetch_add(1, std::memory_order_seq_cst);
    const int hi = g_hi.load(std::memory_order_seq_cst);
    for (int i = 0; i < hi; ++i) {
        Region* r = g_slots[i].load(std::memory_order_seq_cst);
        if (!r) continue;
        const uint64_t lo = std::max(addr, r->base), end = std::min(addr + size, r->base + r->npages * kPage);
        if (lo >= end) continue;
        const uint64_t first = (lo - r->base) / kPage, last = (end - 1 - r->base) / kPage;
        SharedLock lk(r);
        fn(r, first, last - first + 1);
    }
    g_busy.fetch_sub(1, std::memory_order_seq_cst);
}

struct Gauges {
    Gauges() {
        nxrt::gauge_register("mem.ww.faults", [] { return static_cast<int64_t>(g_faults.load(std::memory_order_relaxed)); });
        nxrt::gauge_register("mem.ww.regions", [] { return static_cast<int64_t>(g_live.load(std::memory_order_relaxed)); });
    }
    ~Gauges() {
        nxrt::gauge_unregister("mem.ww.faults");
        nxrt::gauge_unregister("mem.ww.regions");
    }
};
Gauges g_gauges;

}

Region* region_create(uint64_t addr, uint64_t size) {
    if (!size || !heap_exists()) return nullptr;
    const uint64_t start = addr & ~(kPage - 1), end = (addr + size + kPage - 1) & ~(kPage - 1);
    if (start < kHeapBase || end > kHeapBase + heap_reserved()) return nullptr;
    auto r = std::make_unique<Region>();
    r->base = start;
    r->npages = (end - start) / kPage;
    r->nwords = (r->npages + 63) / 64;
    for (auto* bm : {&r->prot, &r->valid, &r->written, &r->ban}) {
        bm->reset(new std::atomic<uint64_t>[r->nwords]);
        for (uint64_t i = 0; i < r->nwords; ++i) (*bm)[i].store(0, std::memory_order_relaxed);
    }
    r->releasing.reset(new std::atomic<uint16_t>[r->npages]);
    for (uint64_t i = 0; i < r->npages; ++i) r->releasing[i].store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_fault_id < 0) g_fault_id = os::fault_register(on_fault, nullptr, os::kPriorityFirst);
        if (g_fault_id < 0) {
            NXLOG_W(kTag, "the fault handler could not be registered: no write tracking");
            return nullptr;
        }
        int slot = -1;
        for (int i = 0; i < kSlots; ++i)
            if (!g_slots[i].load()) { slot = i; break; }
        if (slot < 0) {
            NXLOG_W(kTag, "region table full (%d): no write tracking for %llx", kSlots, static_cast<unsigned long long>(start));
            return nullptr;
        }
        r->slot = slot;
        g_slots[slot].store(r.get(), std::memory_order_seq_cst);
        if (slot >= g_hi.load()) g_hi.store(slot + 1, std::memory_order_seq_cst);
        g_live.fetch_add(1, std::memory_order_seq_cst);
    }
    // Pages that another view shows can never be tracked. Published first, so an alias made from now on bans them through ban_section.
    std::vector<std::pair<uint64_t, uint64_t>> views;
    heap_alias_ranges(views);
    for (const auto& v : views) {
        const uint64_t lo = std::max(start, kHeapBase + v.first), hi = std::min(end, kHeapBase + v.first + v.second);
        if (lo < hi) set_bits(r->ban.get(), (lo - start) / kPage, (hi - lo) / kPage, true);
    }
    return r.release();
}

void region_destroy(Region* r) {
    if (!r) return;
    {
    ExclusiveLock lk(r);
    for (uint64_t p = 0; p < r->npages;) {
        if (!test(r->prot.get(), p)) { ++p; continue; }
        uint64_t q = p;
        while (q < r->npages && test(r->prot.get(), q)) ++q;
        set_bits(r->prot.get(), p, q - p, false);
        os::mem::protect(r->base + p * kPage, (q - p) * kPage, os::mem::Prot::RW, false);
        if (run_budget() != INT64_MAX) g_runs.fetch_sub(1, std::memory_order_relaxed);
        p = q;
    }
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_slots[r->slot].store(nullptr, std::memory_order_seq_cst);
        g_live.fetch_sub(1, std::memory_order_seq_cst);
    }
    while (g_busy.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
    delete r;
}

uint64_t region_base(const Region* r) { return r ? r->base : 0; }
uint64_t region_pages(const Region* r) { return r ? r->npages : 0; }

bool arm(Region* r, uint64_t first, uint64_t count) {
    if (!r || !count || first >= r->npages || count > r->npages - first) return false;
    ExclusiveLock lk(r);
    if (any_set(r->ban.get(), first, count)) return false;
    const uint64_t a = r->base + first * kPage, n = count * kPage;
    // A file read (or a mapping change in memory.cpp) into these pages is in progress: leave them alone. Its scope is published before it
    // releases pages under the shared lock, so either this sees it, or its release waits for this arm and undoes it.
    if (os_write_overlaps(a, a + n)) return false;
    if (g_failed.load(std::memory_order_acquire)) return false;                       // the host ran out of mappings once: everything is "changed"
    // every page committed read-write (the guest may have changed a protection; a page it made read-only is not ours to take)
    for (uint64_t cur = a; cur < a + n;) {
        os::mem::Region q;
        if (!os::mem::query(cur, &q) || q.state != os::mem::State::Committed) return false;
        const uint64_t chunk_end = std::min<uint64_t>(a + n, q.base + q.size);
        const bool already = q.prot == os::mem::Prot::R && all_set(r->prot.get(), (cur - r->base) / kPage, (chunk_end - cur) / kPage);
        if (q.prot != os::mem::Prot::RW && !already) return false;
        cur = chunk_end;
    }
    // Mapping budget (see run_budget): the runs this adds, estimated over the bits around the range. Over budget, recount exactly, then give up.
    int64_t added = 0;
    if (run_budget() != INT64_MAX) {
        const uint64_t lo = first > 0 ? first - 1 : 0, hi = std::min(r->npages, first + count + 1);
        const int64_t before = count_runs(r->prot.get(), lo, hi);
        set_bits(r->prot.get(), first, count, true);
        added = count_runs(r->prot.get(), lo, hi) - before;
        set_bits(r->prot.get(), first, count, false);
        if (g_runs.load(std::memory_order_relaxed) + added > run_budget() * 9 / 10) {
            int64_t exact = 0;
            const int hi_slot = g_hi.load(std::memory_order_seq_cst);
            for (int i = 0; i < hi_slot; ++i)
                if (Region* o = g_slots[i].load(std::memory_order_seq_cst)) exact += count_runs(o->prot.get(), 0, o->npages);
            g_runs.store(exact, std::memory_order_relaxed);
            if (exact + added > run_budget()) return false;
        }
    }
    // Under the exclusive lock no fault is handled until both bits and protection are in place; a write that faults meanwhile waits and
    // then releases the page. A write that lands before the protection does not fault, and the owner's copy (made later) still sees it.
    set_bits(r->prot.get(), first, count, true);
    set_bits(r->valid.get(), first, count, true);
    if (!os::mem::protect(a, n, os::mem::Prot::R, false)) {
        set_bits(r->valid.get(), first, count, false);
        set_bits(r->prot.get(), first, count, false);
        return false;
    }
    if (added) g_runs.fetch_add(added, std::memory_order_relaxed);
    return true;
}

bool all_valid(const Region* r, uint64_t first, uint64_t count) {
    if (!r || !count || first >= r->npages || count > r->npages - first) return false;
    return all_set(r->valid.get(), first, count);
}

bool page_valid(const Region* r, uint64_t page) { return r && page < r->npages && test(r->valid.get(), page); }
bool page_banned(const Region* r, uint64_t page) { return !r || page >= r->npages || test(r->ban.get(), page); }

void invalidate(Region* r, uint64_t first, uint64_t count) {
    if (!r || first >= r->npages) return;
    count = std::min(count, r->npages - first);
    if (count) set_bits(r->valid.get(), first, count, false);
}

void take_written(Region* r, std::vector<uint64_t>& out) {
    if (!r) return;
    for (uint64_t w = 0; w < r->nwords; ++w) {
        if (!r->written[w].load(std::memory_order_relaxed)) continue;
        uint64_t v = r->written[w].exchange(0, std::memory_order_acq_rel);
        while (v) {
            const unsigned long b = static_cast<unsigned long>(__builtin_ctzll(v));
            v &= v - 1;
            const uint64_t page = w * 64 + b;
            if (page < r->npages) out.push_back(page);
        }
    }
}

OsWriteScope::OsWriteScope(const void* p, uint64_t n) {
    if (!n) return;
    const uint64_t lo = reinterpret_cast<uint64_t>(p) & ~(kPage - 1), hi = (reinterpret_cast<uint64_t>(p) + n + kPage - 1) & ~(kPage - 1);
    for (int i = 0; i < kOsSlots && slot_ < 0; ++i) {
        uint64_t expect = 0;
        if (g_os[i].hi.load(std::memory_order_relaxed) == 0 && g_os[i].lo.compare_exchange_strong(expect, lo | 1, std::memory_order_seq_cst)) {
            g_os[i].lo.store(lo, std::memory_order_seq_cst);
            g_os[i].hi.store(hi, std::memory_order_seq_cst);
            slot_ = i;
        }
    }
    if (slot_ < 0) {
        g_os_overflow.fetch_add(1, std::memory_order_seq_cst);
        slot_ = kOsSlots;
    }
    before_os_write(p, n);
}

OsWriteScope::~OsWriteScope() {
    if (slot_ < 0) return;
    if (slot_ == kOsSlots) {
        g_os_overflow.fetch_sub(1, std::memory_order_seq_cst);
        return;
    }
    g_os[slot_].hi.store(0, std::memory_order_seq_cst);
    g_os[slot_].lo.store(0, std::memory_order_seq_cst);
}

void before_os_write(const void* p, uint64_t n) {
    for_regions(reinterpret_cast<uint64_t>(p), n, [](Region* r, uint64_t first, uint64_t count) {
        for (uint64_t q = first; q < first + count; ++q)
            if (test(r->prot.get(), q)) release_page(r, q);
    });
}

void forget(uint64_t addr, uint64_t size, bool ban) {
    for_regions(addr, size, [ban](Region* r, uint64_t first, uint64_t count) {
        if (ban) set_bits(r->ban.get(), first, count, true);
        if (!any_set(r->prot.get(), first, count) && !any_set(r->valid.get(), first, count)) return;
        for (uint64_t q = first; q < first + count; ++q)
            if (test(r->prot.get(), q)) release_page(r, q);
        set_bits(r->valid.get(), first, count, false);
        set_bits(r->written.get(), first, count, true);
    });
}

void ban_section(uint64_t offset, uint64_t size) { forget(kHeapBase + offset, size, true); }

uint64_t faults() { return g_faults.load(std::memory_order_relaxed); }
uint64_t fault_waits() { return g_waits.load(std::memory_order_relaxed); }

}
