// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::mem on Windows (include/nxrt/os/mem.h): VirtualAlloc / VirtualProtect / VirtualQuery, pagefile-backed sections.
#include "nxrt/os/mem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <malloc.h>

#include <algorithm>

namespace nxrt::os::mem {
namespace {

constexpr uint64_t kAddrLimit = 0x800000000000ull;  // user-mode address limit of x64 Windows (47 bits)

DWORD protect_of(Prot p) {
    switch (p) {
        case Prot::R: return PAGE_READONLY;
        case Prot::RW: return PAGE_READWRITE;
        case Prot::RX: return PAGE_EXECUTE_READ;
        case Prot::RWX: return PAGE_EXECUTE_READWRITE;
        case Prot::None: break;
    }
    return PAGE_NOACCESS;
}

Prot prot_from(DWORD p) {
    if (p & PAGE_GUARD) return Prot::None;
    switch (p & 0xFF) {
        case PAGE_READONLY: return Prot::R;
        case PAGE_READWRITE:
        case PAGE_WRITECOPY: return Prot::RW;
        case PAGE_EXECUTE_READ: return Prot::RX;
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY: return Prot::RWX;
        default: break;
    }
    return Prot::None;
}

bool span(uint64_t addr, uint64_t size, uint64_t* start, uint64_t* end) {
    if (size > kAddrLimit || addr > kAddrLimit - size) return false;
    *start = addr & ~(kPage - 1);
    *end = (addr + size + kPage - 1) & ~(kPage - 1);
    return true;
}

}  // namespace

bool reserve_fixed(uint64_t addr, uint64_t size) {
    if (size == 0 || addr % kGranularity != 0 || addr >= kAddrLimit || size > kAddrLimit - addr) return false;
    size = (size + kPage - 1) & ~(kPage - 1);
    void* p = VirtualAlloc(reinterpret_cast<void*>(addr), static_cast<SIZE_T>(size), MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return false;
    if (reinterpret_cast<uint64_t>(p) != addr) {  // cannot happen with MEM_RESERVE at a fixed address; never accept it
        VirtualFree(p, 0, MEM_RELEASE);
        return false;
    }
    return true;
}

bool release(uint64_t addr, uint64_t) { return VirtualFree(reinterpret_cast<void*>(addr), 0, MEM_RELEASE) != 0; }

bool commit(uint64_t addr, uint64_t size, Prot p) {
    uint64_t s = 0, e = 0;
    if (!span(addr, size, &s, &e)) return false;
    if (s == e) return true;
    return VirtualAlloc(reinterpret_cast<void*>(s), static_cast<SIZE_T>(e - s), MEM_COMMIT, protect_of(p)) != nullptr;
}

bool decommit(uint64_t addr, uint64_t size) {
    uint64_t s = 0, e = 0;
    if (!span(addr, size, &s, &e)) return false;
    if (s == e) return true;
    return VirtualFree(reinterpret_cast<void*>(s), static_cast<SIZE_T>(e - s), MEM_DECOMMIT) != 0;
}

bool protect(uint64_t addr, uint64_t size, Prot p, bool verify_committed) {
    uint64_t s = 0, e = 0;
    if (!span(addr, size, &s, &e)) return false;
    if (s == e) return true;
    const DWORD np = protect_of(p);
    if (!verify_committed) {
        DWORD old = 0;
        return VirtualProtect(reinterpret_cast<void*>(s), static_cast<SIZE_T>(e - s), np, &old) != 0;
    }
    for (uint64_t cur = s; cur < e;) {          // every page committed first, so a failure changes nothing
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof mbi) != sizeof mbi || mbi.RegionSize == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        cur = std::min<uint64_t>(e, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize);
    }
    for (uint64_t cur = s; cur < e;) {
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof mbi);
        const uint64_t ce = std::min<uint64_t>(e, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize);
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(cur), static_cast<SIZE_T>(ce - cur), np, &old)) return false;
        cur = ce;
    }
    return true;
}

ProtectResult protect_if(uint64_t addr, uint64_t size, Prot expected, Prot to) {
    uint64_t s = 0, e = 0;
    if (!span(addr, size, &s, &e) || s == e) return ProtectResult::Error;
    DWORD was = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(s), static_cast<SIZE_T>(e - s), protect_of(to), &was)) return ProtectResult::Error;
    if ((was & 0xFF) != protect_of(expected) || (was & PAGE_GUARD)) {     // somebody else's protection: put it back
        DWORD tmp = 0;
        VirtualProtect(reinterpret_cast<void*>(s), static_cast<SIZE_T>(e - s), was, &tmp);
        return ProtectResult::Mismatch;
    }
    return ProtectResult::Ok;
}

void* alloc(uint64_t size, Prot p) {
    return VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_RESERVE | MEM_COMMIT, protect_of(p));
}

bool free(void* p, uint64_t) { return p && VirtualFree(p, 0, MEM_RELEASE) != 0; }

bool query(uint64_t addr, Region* out, uint64_t limit) {
    if (addr >= kAddrLimit || !out) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) != sizeof mbi) return false;
    (void)limit;
    out->base = reinterpret_cast<uint64_t>(mbi.BaseAddress);
    out->size = mbi.RegionSize;
    out->state = mbi.State == MEM_COMMIT ? State::Committed : mbi.State == MEM_RESERVE ? State::Reserved : State::Free;
    out->prot = mbi.State == MEM_COMMIT ? prot_from(mbi.Protect) : Prot::None;
    return true;
}

bool is_accessible(uint64_t addr, uint64_t size) {
    uint64_t s = 0, e = 0;
    if (!span(addr, size, &s, &e)) return false;
    for (uint64_t cur = s; cur < e;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof mbi) != sizeof mbi || mbi.RegionSize == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if ((mbi.Protect & 0xFF) == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
        cur = std::min<uint64_t>(e, reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize);
    }
    return true;
}

bool section_create(uint64_t size, Section* out) {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, static_cast<DWORD>(size >> 32),
                                  static_cast<DWORD>(size & 0xFFFFFFFFu), nullptr);
    if (!h) return false;
    out->native = reinterpret_cast<intptr_t>(h);
    out->size = size;
    return true;
}

void section_close(Section* s) {
    if (s && s->valid()) CloseHandle(reinterpret_cast<HANDLE>(s->native));
    if (s) *s = Section{};
}

uint64_t section_map(const Section& s, uint64_t offset, uint64_t size, uint64_t at, Prot p) {
    if (!s.valid() || size == 0 || offset % kGranularity || size % kGranularity || at % kGranularity || offset > s.size || size > s.size - offset) return 0;
    const DWORD access = (p == Prot::R) ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
    void* v = MapViewOfFileEx(reinterpret_cast<HANDLE>(s.native), access, static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset & 0xFFFFFFFFu),
                              static_cast<SIZE_T>(size), reinterpret_cast<void*>(at));
    if (!v) return 0;
    if (at && reinterpret_cast<uint64_t>(v) != at) {
        UnmapViewOfFile(v);
        return 0;
    }
    return reinterpret_cast<uint64_t>(v);
}

bool section_unmap(uint64_t view, uint64_t) { return UnmapViewOfFile(reinterpret_cast<void*>(view)) != 0; }

void* alloc_aligned(size_t size, size_t align) { return _aligned_malloc(size, align); }
void free_aligned(void* p) { _aligned_free(p); }

uint64_t footprint() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) return 0;
    return static_cast<uint64_t>(pmc.PrivateUsage);
}

uint64_t max_mappings() { return 0; }

}  // namespace nxrt::os::mem
