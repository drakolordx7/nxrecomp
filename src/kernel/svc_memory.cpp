// SPDX-License-Identifier: GPL-3.0-or-later
// Semantics adapted from SuperMarioWonderRecomp runtime/src/kernel.cpp and svc_process.cpp (MIT, Copyright (c) 2026 BelmanteGu); see THIRD_PARTY_NOTICES.md.
// Memory SVC group (include/nxrt/svc.h): heap, MapMemory/UnmapMemory, QueryMemory, permission and attribute calls, shared,
// transfer and physical memory views in the alias region. The address space and the Horizon vm map are in include/nxrt/memory.h.
#include <windows.h>

#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/svc.h"

namespace nxrt {
namespace {

constexpr const char* kTag = "svc";
constexpr uint64_t kHeapGranule = 0x200000;        // SetHeapSize: size must be a multiple of 2 MiB
constexpr uint64_t kHeapOverhead = 0x4000000;       // same overhead GetInfo(UsedMemorySize) adds to the heap
constexpr uint64_t kMaxHeapSvc = 0x100000000ull;   // SetHeapSize: larger than 4 GiB is refused
constexpr uint64_t kPageMask = mem::kPage - 1;
constexpr uint32_t kPermR = 1;                     // svc permission bits, same as mem::vm_set perm bits (bit0 R, bit1 W)
constexpr uint32_t kPermRW = 3;

inline unsigned long long U(uint64_t v) { return v; }

// Guards the heap size, the MapMemory list and the alias-region windows below.
std::mutex g_mu;
uint64_t g_heap_cur = 0;                           // bytes; meaningful while mem::heap_exists()

// MapMemory mappings: dst (stack region) is a view of src (heap) that stays writable only through dst.
struct MapRec {
    uint64_t dst, src, size;
};
std::vector<MapRec> g_maps;

// Host views in the alias region made by MapSharedMemory, MapTransferMemory and MapPhysicalMemory, keyed by their address.
enum class WinKind : uint8_t { Shared, Transfer, Physical };
struct Window {
    uint64_t size;
    WinKind kind;
    uint64_t saved;  // Shared: the object's own buffer while mapped. Transfer: the owner's address. Physical: unused.
};
std::map<uint64_t, Window> g_windows;

uint64_t heap_now() { return mem::heap_exists() ? g_heap_cur : 0; }  // call with g_mu held

// Every byte of [addr, addr+size) lies inside [base, base+len). size is non-zero (callers reject zero sizes first).
bool within(uint64_t addr, uint64_t size, uint64_t base, uint64_t len) {
    return size <= len && addr >= base && addr - base <= len - size;
}
bool in_stack(uint64_t addr, uint64_t size) { return within(addr, size, mem::kStackBase, mem::kStackSize); }
bool in_heap(uint64_t addr, uint64_t size, uint64_t heap) { return within(addr, size, mem::kHeapBase, heap); }

// The map region: where MapSharedMemory / MapTransferMemory / MapPhysicalMemory destinations may be, the ASLR region GetInfo reports
// (svc_info.cpp: 0x8000000 + 0x7FF8000000). The SDK picks random free addresses in it (StackGuardManager::MapAtRandomAddress) and retries
// on a refusal, so any address the host can give out exactly is accepted.
constexpr uint64_t kMapRegionBase = 0x0008000000ull, kMapRegionSize = 0x7FF8000000ull;
bool in_map_region(uint64_t addr, uint64_t size) { return within(addr, size, kMapRegionBase, kMapRegionSize); }

std::map<uint64_t, uint64_t> g_reserve_base;   // window address -> base of its host reservation (differs when addr is not 64 KiB aligned)

// Reserves and commits [addr, addr+size) at exactly addr. A host reservation starts on a 64 KiB boundary, so a page-aligned address is
// served by reserving the enclosing 64 KiB range and committing the pages. False when any of it is taken by something else.
bool alias_alloc(uint64_t addr, uint64_t size) {
    const uint64_t base = addr & ~(mem::kAllocGranularity - 1);
    const uint64_t end = (addr + size + mem::kAllocGranularity - 1) & ~(mem::kAllocGranularity - 1);
    void* p = VirtualAlloc(reinterpret_cast<void*>(base), static_cast<SIZE_T>(end - base), MEM_RESERVE, PAGE_READWRITE);
    if (!p) return false;
    if (reinterpret_cast<uint64_t>(p) != base ||
        !VirtualAlloc(reinterpret_cast<void*>(addr), static_cast<SIZE_T>(size), MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(p, 0, MEM_RELEASE);
        return false;
    }
    g_reserve_base[addr] = base;
    return true;
}

// Releases a window made by alias_alloc (its base address, whole allocation).
void alias_free(uint64_t addr) {
    uint64_t base = addr;
    auto it = g_reserve_base.find(addr);
    if (it != g_reserve_base.end()) {
        base = it->second;
        g_reserve_base.erase(it);
    }
    if (!VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE)) {
        NXLOG_E(kTag, "VirtualFree(%llx) failed, error %lu", U(base), static_cast<unsigned long>(GetLastError()));
    }
}

// Logs the refused call with its argument values and sets the result code.
void fail(::Ctx* c, const char* what, uint32_t rc, uint64_t a, uint64_t b, uint64_t d) {
    NXLOG_W(kTag, "%s returned 0x%X (args %llx, %llx, %llx)", what, rc, U(a), U(b), U(d));
    svc_ret(c, rc);
}

// ---- SetHeapSize (0x01) ------------------------------------------------------------------------------------------------------
void set_heap_size(::Ctx* c) {
    const uint64_t size = reg(c, 1);
    if (size % kHeapGranule) {
        fail(c, "SetHeapSize", kernel::kInvalidSize, size, 0, 0);
        return;
    }
    if (size > kMaxHeapSvc || size > mem::kHeapMax) {
        fail(c, "SetHeapSize", kernel::kOutOfMemory, size, 0, 0);
        return;
    }
    {   // the kernel's resource limit: heap + module images + overhead must fit the physical memory it advertises (GetInfo TotalMemorySize)
        uint64_t images = 0;
        for (Module* m : modules()) images += m->size();
        if (size + images + kHeapOverhead > svc_total_memory()) {
            fail(c, "SetHeapSize", kernel::kOutOfMemory, size, images, svc_total_memory());
            return;
        }
    }
    std::lock_guard lk(g_mu);
    if (!mem::heap_exists()) {
        if (!mem::heap_create(0)) {
            fail(c, "SetHeapSize", kernel::kOutOfMemory, size, 0, 0);
            return;
        }
        g_heap_cur = 0;  // a fresh heap (first call, or after mem::shutdown)
    }
    const uint64_t old = g_heap_cur;
    if (size > old && !mem::heap_commit(old, size - old)) {
        fail(c, "SetHeapSize (commit)", kernel::kOutOfMemory, size, old, 0);
        return;
    }
    if (size < old && !mem::heap_decommit(size, old - size)) {
        fail(c, "SetHeapSize (decommit)", kernel::kInvalidMemoryState, size, old, 0);
        return;
    }
    // Only the changed tail is touched in the vm map, so permissions set by SetMemoryPermission on the rest survive.
    if (size > old) mem::vm_set(mem::kHeapBase + old, size - old, mem::kMemHeap, kPermRW);
    if (size < old) mem::vm_set(mem::kHeapBase + size, old - size, mem::kMemFree, 0);
    g_heap_cur = size;
    reg(c, 1) = mem::kHeapBase;
    NXLOG_D(kTag, "SetHeapSize(%llx) -> %llx (was %llx)", U(size), U(mem::kHeapBase), U(old));
    svc_ret(c, kernel::kOk);
}

// ---- SetMemoryPermission (0x02) ----------------------------------------------------------------------------------------------
void set_memory_permission(::Ctx* c) {
    const uint64_t addr = reg(c, 0), size = reg(c, 1);
    const uint32_t perm = static_cast<uint32_t>(reg(c, 2));
    if (addr & kPageMask) {
        fail(c, "SetMemoryPermission", kernel::kInvalidAddress, addr, size, perm);
        return;
    }
    if (size == 0 || (size & kPageMask)) {
        fail(c, "SetMemoryPermission", kernel::kInvalidSize, addr, size, perm);
        return;
    }
    if (perm != 0 && perm != kPermR && perm != kPermRW) {
        fail(c, "SetMemoryPermission", kernel::kInvalidPermission, addr, size, perm);
        return;
    }
    std::lock_guard lk(g_mu);
    const bool inside = in_heap(addr, size, heap_now()) || in_stack(addr, size);
    if (!inside || !mem::is_mapped(addr, size)) {
        fail(c, "SetMemoryPermission", kernel::kInvalidMemoryState, addr, size, perm);
        return;
    }
    const mem::Prot p = perm == 0 ? mem::Prot::None : (perm == kPermR ? mem::Prot::R : mem::Prot::RW);
    if (!mem::protect(addr, size, p)) {
        fail(c, "SetMemoryPermission (protect)", kernel::kInvalidMemoryState, addr, size, perm);
        return;
    }
    mem::vm_set(addr, size, mem::vm_query(addr).state, perm);
    NXLOG_D(kTag, "SetMemoryPermission(%llx, %llx, %u)", U(addr), U(size), perm);
    svc_ret(c, kernel::kOk);
}

// ---- SetMemoryAttribute (0x03) -----------------------------------------------------------------------------------------------
// Only the Uncached bit (mask 8) with value 0 or 8 is accepted. Anything else is refused; the accepted form is a no-op here.
void set_memory_attribute(::Ctx* c) {
    const uint64_t addr = reg(c, 0), size = reg(c, 1);
    const uint32_t mask = static_cast<uint32_t>(reg(c, 2)), value = static_cast<uint32_t>(reg(c, 3));
    if (addr & kPageMask) {
        fail(c, "SetMemoryAttribute", kernel::kInvalidAddress, addr, size, mask);
        return;
    }
    if (size == 0 || (size & kPageMask)) {
        fail(c, "SetMemoryAttribute", kernel::kInvalidSize, addr, size, mask);
        return;
    }
    if (mask != 8 || (value != 0 && value != 8)) {
        fail(c, "SetMemoryAttribute", kernel::kInvalidPermission, addr, mask, value);
        return;
    }
    NXLOG_D(kTag, "SetMemoryAttribute(%llx, %llx, mask %u, value %u): no-op", U(addr), U(size), mask, value);
    svc_ret(c, kernel::kOk);
}

// ---- MapMemory (0x04) / UnmapMemory (0x05) -----------------------------------------------------------------------------------
void map_memory(::Ctx* c) {
    const uint64_t dst = reg(c, 0), src = reg(c, 1), size = reg(c, 2);
    if ((dst | src) & kPageMask) {
        fail(c, "MapMemory", kernel::kInvalidAddress, dst, src, size);
        return;
    }
    if (size == 0 || (size & kPageMask)) {
        fail(c, "MapMemory", kernel::kInvalidSize, dst, src, size);
        return;
    }
    std::lock_guard lk(g_mu);
    // The source is heap memory or the .data/.bss of a loaded module (state CodeData): the SDK maps thread stacks that live in its
    // own static data (MapAliasStack of a static buffer), which the console allows.
    const mem::VmInfo sv = mem::vm_query(src);
    const bool src_code_data = sv.state == mem::kMemCodeData && src >= sv.addr && src + size <= sv.addr + sv.size;
    if (!in_stack(dst, size) || !(in_heap(src, size, heap_now()) || src_code_data) || !mem::is_mapped(src, size)) {
        fail(c, "MapMemory (range)", kernel::kInvalidMemoryState, dst, src, size);
        return;
    }
    // The destination must be free in the vm map, as on hardware: mapping over a live block would break its bookkeeping.
    const mem::VmInfo d = mem::vm_query(dst);
    if (d.state != mem::kMemFree || d.addr + d.size < dst + size) {
        fail(c, "MapMemory (destination in use)", kernel::kInvalidMemoryState, dst, src, size);
        return;
    }
    if (!mem::commit(dst, size, mem::Prot::RW)) {
        fail(c, "MapMemory (commit)", kernel::kOutOfMemory, dst, src, size);
        return;
    }
    std::memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), size);
    if (!mem::protect(src, size, mem::Prot::None)) {
        mem::decommit(dst, size);
        fail(c, "MapMemory (protect source)", kernel::kInvalidMemoryState, dst, src, size);
        return;
    }
    mem::vm_set(dst, size, mem::kMemStack, kPermRW);
    mem::vm_set(src, size, mem::vm_query(src).state, 0);  // the source is no-access while mapped
    g_maps.push_back(MapRec{dst, src, size});
    NXLOG_D(kTag, "MapMemory(dst %llx, src %llx, size %llx)", U(dst), U(src), U(size));
    svc_ret(c, kernel::kOk);
}

void unmap_memory(::Ctx* c) {
    const uint64_t dst = reg(c, 0), src = reg(c, 1), size = reg(c, 2);
    if ((dst | src) & kPageMask) {
        fail(c, "UnmapMemory", kernel::kInvalidAddress, dst, src, size);
        return;
    }
    if (size == 0 || (size & kPageMask)) {
        fail(c, "UnmapMemory", kernel::kInvalidSize, dst, src, size);
        return;
    }
    std::lock_guard lk(g_mu);
    for (size_t i = 0; i < g_maps.size(); i++) {
        MapRec& r = g_maps[i];
        // A whole mapping, or its first or last part (the rest stays mapped).
        const bool prefix = dst == r.dst && src == r.src && size <= r.size;
        const bool suffix = size <= r.size && dst + size == r.dst + r.size && src + size == r.src + r.size;
        if (!prefix && !suffix) continue;
        // The source is no-access while mapped: it must be made writable before the copy back.
        if (!mem::protect(src, size, mem::Prot::RW)) {
            fail(c, "UnmapMemory (protect source)", kernel::kInvalidMemoryState, dst, src, size);
            return;
        }
        std::memcpy(reinterpret_cast<void*>(src), reinterpret_cast<const void*>(dst), size);
        if (!mem::decommit(dst, size)) NXLOG_E(kTag, "UnmapMemory: decommit(%llx, %llx) failed", U(dst), U(size));
        mem::vm_set(src, size, mem::kMemHeap, kPermRW);
        mem::vm_set(dst, size, mem::kMemFree, 0);
        if (prefix) {
            r.dst += size;
            r.src += size;
        }
        r.size -= size;
        if (r.size == 0) g_maps.erase(g_maps.begin() + static_cast<std::ptrdiff_t>(i));
        NXLOG_D(kTag, "UnmapMemory(dst %llx, src %llx, size %llx)", U(dst), U(src), U(size));
        svc_ret(c, kernel::kOk);
        return;
    }
    fail(c, "UnmapMemory (no such mapping)", kernel::kInvalidMemoryState, dst, src, size);
}

// ---- QueryMemory (0x06) ------------------------------------------------------------------------------------------------------
// Writes the 0x28-byte MemoryInfo at x0; x1 (PageInfo) is 0.
struct MemoryInfo {
    uint64_t base_address;
    uint64_t size;
    uint32_t type;
    uint32_t attribute;
    uint32_t permission;
    uint32_t ipc_refcount;
    uint32_t device_refcount;
    uint32_t padding;
};
static_assert(sizeof(MemoryInfo) == 0x28, "MemoryInfo is 0x28 bytes");

void query_memory(::Ctx* c) {
    const uint64_t out = reg(c, 0), addr = reg(c, 2);
    if (!out) {
        fail(c, "QueryMemory", kernel::kInvalidAddress, out, addr, 0);
        return;
    }
    const mem::VmInfo vi = mem::vm_query(addr);
    MemoryInfo info{};
    info.base_address = vi.addr;
    info.size = vi.size;
    info.type = vi.state;
    info.permission = vi.perm;
    std::memcpy(reinterpret_cast<void*>(out), &info, sizeof info);
    reg(c, 1) = 0;
    NXLOG_D(kTag, "QueryMemory(%llx) -> base %llx size %llx type %u perm %u", U(addr), U(vi.addr), U(vi.size), vi.state,
            vi.perm);
    svc_ret(c, kernel::kOk);
}

// ---- MapSharedMemory (0x13) / UnmapSharedMemory (0x14) ----------------------------------------------------------------------
// The guest picks the address in the alias region. The object's buffer is copied to it and the object then works on that view
// (s->host = addr), so services write straight into the guest's copy. Unmap copies the view back into the object's own buffer.
void map_shared_memory(::Ctx* c) {
    const uint32_t h = static_cast<uint32_t>(reg(c, 0));
    const uint64_t addr = reg(c, 1), size = reg(c, 2);
    const uint32_t perm = static_cast<uint32_t>(reg(c, 3));
    auto s = kernel::handle_as<kernel::KSharedMemory>(h);
    if (!s) {
        fail(c, "MapSharedMemory (handle)", kernel::kInvalidHandle, h, addr, size);
        return;
    }
    if (addr & kPageMask) {
        fail(c, "MapSharedMemory", kernel::kInvalidAddress, addr, size, h);
        return;
    }
    if (size == 0 || (size & kPageMask) || size > s->size) {
        fail(c, "MapSharedMemory", kernel::kInvalidSize, addr, size, s->size);
        return;
    }
    std::lock_guard lk(g_mu);
    if (!in_map_region(addr, size) || !alias_alloc(addr, size)) {
        fail(c, "MapSharedMemory (address)", kernel::kInvalidMemoryState, addr, size, h);
        return;
    }
    std::memcpy(reinterpret_cast<void*>(addr), reinterpret_cast<const void*>(s->host), size);
    g_windows[addr] = Window{size, WinKind::Shared, s->host};
    s->host = addr;
    mem::vm_set(addr, size, mem::kMemShared, perm);
    NXLOG_D(kTag, "MapSharedMemory(handle %x, addr %llx, size %llx, perm %u)", h, U(addr), U(size), perm);
    svc_ret(c, kernel::kOk);
}

void unmap_shared_memory(::Ctx* c) {
    const uint32_t h = static_cast<uint32_t>(reg(c, 0));
    const uint64_t addr = reg(c, 1), size = reg(c, 2);
    auto s = kernel::handle_as<kernel::KSharedMemory>(h);
    if (!s) {
        fail(c, "UnmapSharedMemory (handle)", kernel::kInvalidHandle, h, addr, size);
        return;
    }
    if (addr & kPageMask) {
        fail(c, "UnmapSharedMemory", kernel::kInvalidAddress, addr, size, h);
        return;
    }
    if (size == 0 || (size & kPageMask)) {
        fail(c, "UnmapSharedMemory", kernel::kInvalidSize, addr, size, h);
        return;
    }
    std::lock_guard lk(g_mu);
    auto it = g_windows.find(addr);
    if (it == g_windows.end() || it->second.kind != WinKind::Shared || it->second.size != size || s->host != addr) {
        fail(c, "UnmapSharedMemory (not mapped here)", kernel::kInvalidMemoryState, addr, size, h);
        return;
    }
    // Copy the view back into the object's own buffer (kept since MapSharedMemory), then point the object at it again.
    std::memcpy(reinterpret_cast<void*>(it->second.saved), reinterpret_cast<const void*>(addr), size);
    s->host = it->second.saved;
    alias_free(addr);
    g_windows.erase(it);
    mem::vm_set(addr, size, mem::kMemFree, 0);
    NXLOG_D(kTag, "UnmapSharedMemory(handle %x, addr %llx, size %llx)", h, U(addr), U(size));
    svc_ret(c, kernel::kOk);
}

// ---- CreateSharedMemory (0x50) -----------------------------------------------------------------------------------------------
// x1 size (rounded up to a page), w2 owner permission, w3 remote permission (not enforced); handle in x1.
void create_shared_memory(::Ctx* c) {
    const uint64_t size = reg(c, 1);
    if (size == 0) {
        fail(c, "CreateSharedMemory", kernel::kInvalidSize, size, 0, 0);
        return;
    }
    if (size > mem::kKernelSize) {
        fail(c, "CreateSharedMemory", kernel::kOutOfMemory, size, 0, 0);
        return;
    }
    const uint64_t rounded = (size + kPageMask) & ~kPageMask;
    const uint64_t host = mem::alloc_kernel(rounded);
    if (!host) {
        fail(c, "CreateSharedMemory (alloc)", kernel::kOutOfMemory, size, 0, 0);
        return;
    }
    auto s = std::make_shared<kernel::KSharedMemory>();
    s->size = rounded;
    s->host = host;
    const uint32_t h = kernel::handle_add(s);
    if (!h) {
        fail(c, "CreateSharedMemory (handle table full)", kernel::kOutOfResource, size, 0, 0);
        return;
    }
    reg(c, 1) = h;
    NXLOG_D(kTag, "CreateSharedMemory(size %llx) -> handle %x", U(rounded), h);
    svc_ret(c, kernel::kOk);
}

// ---- CreateTransferMemory (0x15) ---------------------------------------------------------------------------------------------
// x1 owner address, x2 size, w3 permission (recorded by the guest, not enforced); handle in x1.
void create_transfer_memory(::Ctx* c) {
    auto tm = std::make_shared<kernel::KTransferMemory>();
    tm->addr = reg(c, 1);
    tm->size = reg(c, 2);
    const uint32_t h = kernel::handle_add(tm);
    if (!h) {
        fail(c, "CreateTransferMemory (handle table full)", kernel::kOutOfResource, tm->addr, tm->size, 0);
        return;
    }
    reg(c, 1) = h;
    NXLOG_D(kTag, "CreateTransferMemory(addr %llx, size %llx) -> handle %x", U(tm->addr), U(tm->size), h);
    svc_ret(c, kernel::kOk);
}

// ---- MapTransferMemory (0x51) / UnmapTransferMemory (0x52) -------------------------------------------------------------------
// The owner's bytes are copied to the view on map and copied back on unmap (the owner cannot touch them while it is mapped).
void map_transfer_memory(::Ctx* c) {
    const uint32_t h = static_cast<uint32_t>(reg(c, 0));
    const uint64_t addr = reg(c, 1), size = reg(c, 2);
    const uint32_t perm = static_cast<uint32_t>(reg(c, 3));
    auto tm = kernel::handle_as<kernel::KTransferMemory>(h);
    if (!tm) {
        fail(c, "MapTransferMemory (handle)", kernel::kInvalidHandle, h, addr, size);
        return;
    }
    if (addr & kPageMask) {
        fail(c, "MapTransferMemory", kernel::kInvalidAddress, addr, size, h);
        return;
    }
    if (size == 0 || (size & kPageMask) || size > tm->size) {
        fail(c, "MapTransferMemory", kernel::kInvalidSize, addr, size, tm->size);
        return;
    }
    std::lock_guard lk(g_mu);
    if (!in_map_region(addr, size) || !mem::is_mapped(tm->addr, size) || !alias_alloc(addr, size)) {
        fail(c, "MapTransferMemory (address or owner)", kernel::kInvalidMemoryState, addr, tm->addr, size);
        return;
    }
    std::memcpy(reinterpret_cast<void*>(addr), reinterpret_cast<const void*>(tm->addr), size);
    g_windows[addr] = Window{size, WinKind::Transfer, tm->addr};
    mem::vm_set(addr, size, mem::kMemTransfer, perm);
    NXLOG_D(kTag, "MapTransferMemory(handle %x, addr %llx, size %llx, perm %u)", h, U(addr), U(size), perm);
    svc_ret(c, kernel::kOk);
}

void unmap_transfer_memory(::Ctx* c) {
    const uint32_t h = static_cast<uint32_t>(reg(c, 0));
    const uint64_t addr = reg(c, 1), size = reg(c, 2);
    auto tm = kernel::handle_as<kernel::KTransferMemory>(h);
    if (!tm) {
        fail(c, "UnmapTransferMemory (handle)", kernel::kInvalidHandle, h, addr, size);
        return;
    }
    if (addr & kPageMask) {
        fail(c, "UnmapTransferMemory", kernel::kInvalidAddress, addr, size, h);
        return;
    }
    if (size == 0 || (size & kPageMask)) {
        fail(c, "UnmapTransferMemory", kernel::kInvalidSize, addr, size, h);
        return;
    }
    std::lock_guard lk(g_mu);
    auto it = g_windows.find(addr);
    if (it == g_windows.end() || it->second.kind != WinKind::Transfer || it->second.size != size ||
        it->second.saved != tm->addr) {
        fail(c, "UnmapTransferMemory (not mapped here)", kernel::kInvalidMemoryState, addr, size, h);
        return;
    }
    if (!mem::is_mapped(tm->addr, size)) {
        fail(c, "UnmapTransferMemory (owner gone)", kernel::kInvalidMemoryState, addr, tm->addr, size);
        return;
    }
    std::memcpy(reinterpret_cast<void*>(tm->addr), reinterpret_cast<const void*>(addr), size);
    alias_free(addr);
    g_windows.erase(it);
    mem::vm_set(addr, size, mem::kMemFree, 0);
    NXLOG_D(kTag, "UnmapTransferMemory(handle %x, addr %llx, size %llx)", h, U(addr), U(size));
    svc_ret(c, kernel::kOk);
}

// ---- MapPhysicalMemory (0x2C) / UnmapPhysicalMemory (0x2D) -------------------------------------------------------------------
// Host memory committed at a free address of the alias region. Unmap releases the whole reservation so the range can be mapped again.
void map_physical_memory(::Ctx* c) {
    const uint64_t addr = reg(c, 0), size = reg(c, 1);
    if (addr & kPageMask) {
        fail(c, "MapPhysicalMemory", kernel::kInvalidAddress, addr, size, 0);
        return;
    }
    if (size == 0 || (size & kPageMask)) {
        fail(c, "MapPhysicalMemory", kernel::kInvalidSize, addr, size, 0);
        return;
    }
    std::lock_guard lk(g_mu);
    if (!in_map_region(addr, size) || !alias_alloc(addr, size)) {
        fail(c, "MapPhysicalMemory (address range or already mapped)", kernel::kInvalidMemoryState, addr, size, 0);
        return;
    }
    g_windows[addr] = Window{size, WinKind::Physical, 0};
    mem::vm_set(addr, size, mem::kMemAlias, kPermRW);
    NXLOG_D(kTag, "MapPhysicalMemory(addr %llx, size %llx)", U(addr), U(size));
    svc_ret(c, kernel::kOk);
}

void unmap_physical_memory(::Ctx* c) {
    const uint64_t addr = reg(c, 0), size = reg(c, 1);
    if (addr & kPageMask) {
        fail(c, "UnmapPhysicalMemory", kernel::kInvalidAddress, addr, size, 0);
        return;
    }
    if (size == 0 || (size & kPageMask)) {
        fail(c, "UnmapPhysicalMemory", kernel::kInvalidSize, addr, size, 0);
        return;
    }
    std::lock_guard lk(g_mu);
    auto it = g_windows.find(addr);
    if (it == g_windows.end() || it->second.kind != WinKind::Physical || it->second.size != size) {
        fail(c, "UnmapPhysicalMemory (not mapped)", kernel::kInvalidMemoryState, addr, size, 0);
        return;
    }
    alias_free(addr);
    g_windows.erase(it);
    mem::vm_set(addr, size, mem::kMemFree, 0);
    NXLOG_D(kTag, "UnmapPhysicalMemory(addr %llx, size %llx)", U(addr), U(size));
    svc_ret(c, kernel::kOk);
}

// ---- cache maintenance (0x2A, 0x2B): nothing to do on the host -------------------------------------------------------------
void flush_entire_data_cache(::Ctx* c) {
    NXLOG_D(kTag, "FlushEntireDataCache: no-op");
    svc_ret(c, kernel::kOk);
}

void flush_data_cache(::Ctx* c) {
    NXLOG_D(kTag, "FlushDataCache(%llx, %llx): no-op", U(reg(c, 0)), U(reg(c, 1)));
    svc_ret(c, kernel::kOk);
}

}  // namespace

uint64_t svc_current_heap_size() {
    std::lock_guard lk(g_mu);
    return heap_now();
}

void svc_register_memory_group() {
    svc_register(0x01, set_heap_size);
    svc_register(0x02, set_memory_permission);
    svc_register(0x03, set_memory_attribute);
    svc_register(0x04, map_memory);
    svc_register(0x05, unmap_memory);
    svc_register(0x06, query_memory);
    svc_register(0x13, map_shared_memory);
    svc_register(0x14, unmap_shared_memory);
    svc_register(0x15, create_transfer_memory);
    svc_register(0x2A, flush_entire_data_cache);
    svc_register(0x2B, flush_data_cache);
    svc_register(0x2C, map_physical_memory);
    svc_register(0x2D, unmap_physical_memory);
    svc_register(0x50, create_shared_memory);
    svc_register(0x51, map_transfer_memory);
    svc_register(0x52, unmap_transfer_memory);
}

// Tests only: releases every alias-region window and forgets the MapMemory list and the heap size. The heap itself and
// the vm map are reset by mem::shutdown().
void svc_memory_reset_for_tests() {
    std::lock_guard lk(g_mu);
    for (auto& kv : g_windows) alias_free(kv.first);
    g_windows.clear();
    g_maps.clear();
    g_heap_cur = 0;
}

}  // namespace nxrt
