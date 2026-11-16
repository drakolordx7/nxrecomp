// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::mem on Linux (include/nxrt/os/mem.h): mmap MAP_FIXED_NOREPLACE, mprotect, memfd_create sections, and a shadow page table for the
// guest span so query()/is_accessible()/protect_if() are plain array reads (never /proc/self/maps, and safe inside a signal handler).
//
// The shadow table has one byte per 4 KiB page of [kTrackedLo, kTrackedHi): 0 free, 1 reserved, 2 + Prot committed. It is written by whoever
// changes a mapping (no lock: concurrent changes of the same page are the caller's business, as with VirtualProtect) and read by anybody.
// It is allocated lazily (MAP_NORESERVE, zero pages cost nothing until written).
#include "nxrt/os/mem.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace nxrt::os::mem {
namespace {

constexpr uint64_t kAddrLimit = 0x800000000000ull;
constexpr uint64_t kShadowPages = (kTrackedHi - kTrackedLo) / kPage;
constexpr uint8_t kFree = 0, kReserved = 1, kCommittedBase = 2;

std::atomic<std::atomic<uint8_t>*> g_shadow{nullptr};
std::once_flag g_shadow_once;

std::atomic<uint8_t>* shadow_alloc() {
    std::call_once(g_shadow_once, [] {
        void* p = mmap(nullptr, kShadowPages, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p != MAP_FAILED) g_shadow.store(static_cast<std::atomic<uint8_t>*>(p), std::memory_order_release);
    });
    return g_shadow.load(std::memory_order_acquire);
}

std::atomic<uint8_t>* shadow() { return g_shadow.load(std::memory_order_acquire); }

bool tracked(uint64_t addr) { return addr >= kTrackedLo && addr < kTrackedHi; }
// [s, e) entirely inside the tracked window
bool tracked_range(uint64_t s, uint64_t e) { return s >= kTrackedLo && e <= kTrackedHi && s < e; }

uint8_t code_of(Prot p) { return static_cast<uint8_t>(kCommittedBase + static_cast<uint8_t>(p)); }

// Records `code` for the pages of [s, e) that lie in the window.
void shadow_set(uint64_t s, uint64_t e, uint8_t code) {
    std::atomic<uint8_t>* sh = shadow_alloc();
    if (!sh) return;
    s = std::max(s, kTrackedLo);
    e = std::min(e, kTrackedHi);
    for (uint64_t a = s; a < e; a += kPage) sh[(a - kTrackedLo) / kPage].store(code, std::memory_order_relaxed);
}

int prot_flags(Prot p) {
    switch (p) {
        case Prot::R: return PROT_READ;
        case Prot::RW: return PROT_READ | PROT_WRITE;
        case Prot::RX: return PROT_READ | PROT_EXEC;
        case Prot::RWX: return PROT_READ | PROT_WRITE | PROT_EXEC;
        case Prot::None: break;
    }
    return PROT_NONE;
}

Prot prot_from_perms(const char* perms) {
    const bool r = perms[0] == 'r', w = perms[1] == 'w', x = perms[2] == 'x';
    if (!r && !w && !x) return Prot::None;
    if (w && x) return Prot::RWX;
    if (w) return Prot::RW;
    if (x) return Prot::RX;
    return Prot::R;
}

bool span(uint64_t addr, uint64_t size, uint64_t* s, uint64_t* e) {
    if (size > kAddrLimit || addr > kAddrLimit - size) return false;
    *s = addr & ~(kPage - 1);
    *e = (addr + size + kPage - 1) & ~(kPage - 1);
    return true;
}

// Reads /proc/self/maps for the mapping holding `addr` (or the gap around it). Not signal safe; for addresses outside the shadow window.
bool query_maps(uint64_t addr, Region* out) {
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    uint64_t prev_end = 0;
    bool found = false;
    while (std::fgets(line, sizeof line, f)) {
        unsigned long long lo = 0, hi = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
        if (addr < lo) {                          // the gap before this mapping holds addr
            out->base = prev_end;
            out->size = lo - prev_end;
            out->state = State::Free;
            out->prot = Prot::None;
            found = true;
            break;
        }
        if (addr < hi) {
            const Prot p = prot_from_perms(perms);
            out->base = lo;
            out->size = hi - lo;
            out->state = p == Prot::None ? State::Reserved : State::Committed;
            out->prot = p;
            found = true;
            break;
        }
        prev_end = hi;
    }
    if (!found) {
        out->base = prev_end;
        out->size = kAddrLimit > prev_end ? kAddrLimit - prev_end : 0;
        out->state = State::Free;
        out->prot = Prot::None;
    }
    std::fclose(f);
    return true;
}

}  // namespace

bool reserve_fixed(uint64_t addr, uint64_t size) {
    if (size == 0 || addr % kGranularity != 0 || addr >= kAddrLimit || size > kAddrLimit - addr) return false;
    size = (size + kPage - 1) & ~(kPage - 1);
    void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (reinterpret_cast<uint64_t>(p) != addr) {   // a kernel older than 4.17 treats the flag as a hint: never accept another address
        munmap(p, size);
        return false;
    }
    shadow_set(addr, addr + size, kReserved);
    return true;
}

bool release(uint64_t addr, uint64_t size) {
    uint64_t s = 0, e = 0;
    if (!span(addr, size, &s, &e)) return false;
    if (munmap(reinterpret_cast<void*>(s), e - s) != 0) return false;
    shadow_set(s, e, kFree);
    return true;
}

bool commit(uint64_t addr, uint64_t size, Prot p) {
    uint64_t s = 0, e = 0;
    if (!span(addr, size, &s, &e)) return false;
    if (s == e) return true;
    if (mprotect(reinterpret_cast<void*>(s), e - s, prot_flags(p)) != 0) return false;
    shadow_set(s, e, code_of(p));
    return true;
}

bool decommit(uint64_t addr, uint64_t size) {
    uint64_t s = 0, e = 0;
    if (!span(addr, size, &s, &e)) return false;
    if (s == e) return true;
    if (mprotect(reinterpret_cast<void*>(s), e - s, PROT_NONE) != 0) return false;
    madvise(reinterpret_cast<void*>(s), e - s, MADV_DONTNEED);     // private pages go back to the kernel and read zero when committed again
    shadow_set(s, e, kReserved);
    return true;
}

bool protect(uint64_t addr, uint64_t size, Prot p, bool verify_committed) {
    uint64_t s = 0, e = 0;
    if (!span(addr, size, &s, &e)) return false;
    if (s == e) return true;
    if (verify_committed && tracked_range(s, e)) {
        std::atomic<uint8_t>* sh = shadow();
        if (!sh) return false;
        for (uint64_t a = s; a < e; a += kPage)
            if (sh[(a - kTrackedLo) / kPage].load(std::memory_order_relaxed) < kCommittedBase) return false;   // not committed: Windows refuses too
    }
    if (mprotect(reinterpret_cast<void*>(s), e - s, prot_flags(p)) != 0) return false;
    shadow_set(s, e, code_of(p));
    return true;
}

ProtectResult protect_if(uint64_t addr, uint64_t size, Prot expected, Prot to) {
    uint64_t s = 0, e = 0;
    if (!span(addr, size, &s, &e) || s == e || !tracked_range(s, e)) return ProtectResult::Error;
    std::atomic<uint8_t>* sh = shadow();
    if (!sh) return ProtectResult::Error;
    const uint8_t want = code_of(expected);
    for (uint64_t a = s; a < e; a += kPage)
        if (sh[(a - kTrackedLo) / kPage].load(std::memory_order_acquire) != want) return ProtectResult::Mismatch;
    if (mprotect(reinterpret_cast<void*>(s), e - s, prot_flags(to)) != 0) return errno == ENOMEM ? ProtectResult::NoMem : ProtectResult::Error;
    const uint8_t code = code_of(to);
    for (uint64_t a = s; a < e; a += kPage) sh[(a - kTrackedLo) / kPage].store(code, std::memory_order_release);
    return ProtectResult::Ok;
}

void* alloc(uint64_t size, Prot p) {
    void* v = mmap(nullptr, size, prot_flags(p), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return v == MAP_FAILED ? nullptr : v;
}

bool free(void* p, uint64_t size) { return p && munmap(p, size) == 0; }

bool query(uint64_t addr, Region* out, uint64_t limit) {
    if (addr >= kAddrLimit || !out) return false;
    if (!tracked(addr)) return query_maps(addr, out);
    std::atomic<uint8_t>* sh = shadow();
    const uint64_t page0 = (addr - kTrackedLo) / kPage;
    const uint8_t c = sh ? sh[page0].load(std::memory_order_acquire) : kFree;
    uint64_t lo = page0, hi = page0 + 1;
    const uint64_t max_hi = limit == ~0ull ? kShadowPages : std::min<uint64_t>(kShadowPages, page0 + 1 + limit / kPage);
    if (sh) {
        while (hi < max_hi && sh[hi].load(std::memory_order_relaxed) == c) ++hi;
        if (limit == ~0ull)
            while (lo > 0 && sh[lo - 1].load(std::memory_order_relaxed) == c) --lo;
    }
    out->base = kTrackedLo + lo * kPage;
    out->size = (hi - lo) * kPage;
    out->state = c == kFree ? State::Free : c == kReserved ? State::Reserved : State::Committed;
    out->prot = c >= kCommittedBase ? static_cast<Prot>(c - kCommittedBase) : Prot::None;
    return true;
}

bool is_accessible(uint64_t addr, uint64_t size) {
    uint64_t s = 0, e = 0;
    if (!span(addr, size, &s, &e)) return false;
    if (tracked_range(s, e)) {
        std::atomic<uint8_t>* sh = shadow();
        if (!sh) return false;
        for (uint64_t a = s; a < e; a += kPage)
            if (sh[(a - kTrackedLo) / kPage].load(std::memory_order_acquire) < kCommittedBase + 1) return false;   // free, reserved or Prot::None
        return true;
    }
    for (uint64_t cur = s; cur < e;) {
        Region r;
        if (!query(cur, &r) || r.state != State::Committed || r.prot == Prot::None || r.size == 0) return false;
        cur = r.base + r.size;
    }
    return true;
}

bool section_create(uint64_t size, Section* out) {
    int fd = memfd_create("nx-section", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd);
        return false;
    }
    out->native = fd;
    out->size = size;
    return true;
}

void section_close(Section* s) {
    if (s && s->valid()) close(static_cast<int>(s->native));
    if (s) *s = Section{};
}

uint64_t section_map(const Section& s, uint64_t offset, uint64_t size, uint64_t at, Prot p) {
    if (!s.valid() || size == 0 || offset % kGranularity || size % kGranularity || at % kGranularity || offset > s.size || size > s.size - offset) return 0;
    int flags = MAP_SHARED | MAP_NORESERVE;
    if (at) flags |= MAP_FIXED_NOREPLACE;
    void* v = mmap(reinterpret_cast<void*>(at), size, prot_flags(p), flags, static_cast<int>(s.native), static_cast<off_t>(offset));
    if (v == MAP_FAILED) return 0;
    if (at && reinterpret_cast<uint64_t>(v) != at) {
        munmap(v, size);
        return 0;
    }
    const uint64_t a = reinterpret_cast<uint64_t>(v);
    shadow_set(a, a + size, p == Prot::None ? kReserved : code_of(p));
    return a;
}

bool section_unmap(uint64_t view, uint64_t size) {
    if (munmap(reinterpret_cast<void*>(view), size) != 0) return false;
    shadow_set(view, view + size, kFree);
    return true;
}

void* alloc_aligned(size_t size, size_t align) {
    void* p = nullptr;
    if (align < sizeof(void*)) align = sizeof(void*);
    return posix_memalign(&p, align, size) == 0 ? p : nullptr;
}

void free_aligned(void* p) { std::free(p); }

uint64_t footprint() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    uint64_t kb = 0;
    while (std::fgets(line, sizeof line, f)) {
        unsigned long long v = 0;
        if (std::sscanf(line, "RssAnon: %llu kB", &v) == 1) kb += v;
        else if (std::sscanf(line, "RssShmem: %llu kB", &v) == 1) kb += v;
    }
    std::fclose(f);
    return kb * 1024;
}

uint64_t max_mappings() {
    static const uint64_t v = [] {
        FILE* f = std::fopen("/proc/sys/vm/max_map_count", "r");
        if (!f) return uint64_t{65530};
        unsigned long long n = 0;
        if (std::fscanf(f, "%llu", &n) != 1) n = 65530;
        std::fclose(f);
        return static_cast<uint64_t>(n);
    }();
    return v;
}

}  // namespace nxrt::os::mem
