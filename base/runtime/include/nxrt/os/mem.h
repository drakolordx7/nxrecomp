// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::mem: address-space primitives of the host (src/os/win32/mem.cpp, src/os/posix/mem.cpp). The guest memory manager
// (nxrt/memory.h) and everything that needs raw pages is built on these; no other file calls VirtualAlloc, mmap, mprotect and friends.
//
// Model (the same on both systems):
//   free      nothing there.
//   reserved  address space owned by us, any access faults.
//   committed backed by memory; accessible as its Prot says (Prot::None = committed but any access faults, kept for section views whose
//             pages cannot be decommitted).
// Addresses and sizes are byte counts; the unit of change is the 4 KiB page (kPage), the unit of *placement* (reserve_fixed, section views)
// is kGranularity (64 KiB, the Windows rule, kept on Linux so a layout that works on one works on the other).
// Nothing here takes a lock the signal/exception path needs: query() and protect_if() may be called from a fault handler (see fault.h).
// Nothing here logs; callers log failures with their own context.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::os::mem {

constexpr uint64_t kPage = 0x1000;
constexpr uint64_t kGranularity = 0x10000;

enum class Prot : uint8_t { None, R, RW, RX, RWX };

// The part of the address space the posix implementation keeps a shadow page table for (one byte per page, so query() and is_accessible()
// never read /proc/self/maps there). It is the guest span of nxrt/memory.h; the numbers are repeated here because the os layer does not
// include the runtime. Outside it, query() still works on Linux (slower, reads /proc/self/maps, not signal safe).
constexpr uint64_t kTrackedLo = 0x7100000000ull;
constexpr uint64_t kTrackedHi = 0x7F00000000ull;

// ---- reserve / commit / protect ------------------------------------------------------------------------------------------------
// Reserve [addr, addr + size) at exactly addr (both multiples of kGranularity / kPage rules above: addr aligned to kGranularity, size rounded
// up to kPage). Never replaces an existing mapping: false when any byte of the range is taken.
bool reserve_fixed(uint64_t addr, uint64_t size);
// Gives a reservation made by reserve_fixed back, committed pages included. `addr` is the start and `size` the size it was reserved with.
bool release(uint64_t addr, uint64_t size);
// Makes pages of a reservation (or already committed pages: the protection changes, the contents stay) accessible as `p`. New pages read zero.
bool commit(uint64_t addr, uint64_t size, Prot p);
// Pages go back to "reserved": contents dropped, accesses fault. Not for pages of a section view (those use protect(.., Prot::None)).
bool decommit(uint64_t addr, uint64_t size);
// Changes the protection of committed pages. verify_committed: first check that every page is committed and refuse otherwise (a failed call then
// changes nothing); false skips that check for callers that know the range is committed (the check walks the host's page table: slow over a
// multi-gigabyte section view on Windows).
bool protect(uint64_t addr, uint64_t size, Prot p, bool verify_committed = true);

// For a fault handler: if [addr, addr + size) is committed with protection `expected` everywhere, changes it to `to`. Mismatch means the
// range was not `expected` (nothing changed); NoMem is a host mapping limit (Linux vm.max_map_count): the page could not be split off.
enum class ProtectResult : uint8_t { Ok, Mismatch, NoMem, Error };
ProtectResult protect_if(uint64_t addr, uint64_t size, Prot expected, Prot to);

// One address anywhere (the system picks), committed, for tests and tools. Not tracked, not inside the guest span.
void* alloc(uint64_t size, Prot p = Prot::RW);
bool free(void* p, uint64_t size);

// ---- query ---------------------------------------------------------------------------------------------------------------------
enum class State : uint8_t { Free, Reserved, Committed };
struct Region {
    uint64_t base = 0, size = 0;    // the run of pages around the queried address that have the same state and protection
    State state = State::Free;
    Prot prot = Prot::None;         // meaningful when state is Committed
};
// Describes the run containing `addr` (page granularity), looking at most `limit` bytes past the page of addr (a short limit keeps it cheap
// inside a hot path). False when addr is outside the user address space.
bool query(uint64_t addr, Region* out, uint64_t limit = ~0ull);
// Every byte of [addr, addr + size) is committed and not Prot::None (read-only counts as accessible).
bool is_accessible(uint64_t addr, uint64_t size);

// ---- shared-memory sections ----------------------------------------------------------------------------------------------------
// A section is anonymous shared memory of a fixed size (Windows: pagefile-backed SEC_RESERVE section, Linux: memfd). Views map a range of it
// into the address space; several views of the same range show the same bytes. Nothing is charged until a page is touched (Linux) or
// committed (Windows: pages of a section start reserved; commit() on a view makes them usable through every view).
struct Section {
    intptr_t native = -1;           // HANDLE or file descriptor: opaque to callers
    uint64_t size = 0;
    bool valid() const { return native != -1 && native != 0; }
};
bool section_create(uint64_t size, Section* out);
// Closes the handle. Existing views stay mapped until section_unmap.
void section_close(Section* s);
// Maps [offset, offset + size) at exactly `at` (a multiple of kGranularity; the range must be free), or at an address the system picks when at
// is 0. offset and size are multiples of kGranularity. Prot::None gives a view whose pages are reserved until commit() (use it for the first
// view of a section the caller commits piecewise); any other Prot gives pages usable at once (Prot::R: a read-only view). Returns the view address
// or 0.
uint64_t section_map(const Section& s, uint64_t offset, uint64_t size, uint64_t at, Prot p);
// Unmaps a view made by section_map (same address and size); the range is free again.
bool section_unmap(uint64_t view, uint64_t size);

// ---- aligned heap memory (replaces _aligned_malloc / _aligned_free) -----------------------------------------------------------------
void* alloc_aligned(size_t size, size_t align);
void free_aligned(void* p);

// ---- process facts --------------------------------------------------------------------------------------------------------------
// Memory this process holds that is not shared with the file system cache: Windows PrivateUsage, Linux anonymous + shmem resident pages.
// 0 when the system will not say.
uint64_t footprint();
// The most separate mappings (VMAs) the process may hold: Linux vm.max_map_count; 0 on Windows (no such limit). A page protection change in
// the middle of a mapping splits it, so callers that make many scattered read-only pages (write_watch.cpp) stay well below this.
uint64_t max_mappings();

}  // namespace nxrt::os::mem
