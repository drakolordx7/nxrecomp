// SPDX-License-Identifier: GPL-3.0-or-later
// S38: page write tracking (include/nxrt/write_watch.h).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/memory.h"
#include "nxrt/write_watch.h"
#include "test_framework.h"

NXTEST_GROUP(write_watch)

namespace {

namespace mem = nxrt::mem;
namespace ww = nxrt::mem::ww;
constexpr uint64_t kPg = 4096;
constexpr uint64_t kHeap = 64ull << 20;
constexpr uint64_t kPages = 64;

DWORD prot_of(uint64_t addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi);
    return mbi.Protect & 0xff;
}

// A fresh heap with kPages committed pages at offset 0 and a region over them.
ww::Region* setup() {
    mem::shutdown();
    if (!mem::init() || !mem::heap_create(kHeap) || !mem::heap_commit(0, kPages * kPg)) return nullptr;
    return ww::region_create(mem::kHeapBase, kPages * kPg);
}

void teardown(ww::Region* r) {
    ww::region_destroy(r);
    mem::shutdown();
}

}  // namespace

NXTEST(arm_then_write_releases_the_page) {
    ww::Region* r = setup();
    REQUIRE(r != nullptr);
    CHECK_EQ(ww::region_pages(r), kPages);
    CHECK(ww::arm(r, 2, 4));
    CHECK(ww::all_valid(r, 2, 4));
    CHECK_EQ(prot_of(mem::kHeapBase + 3 * kPg), static_cast<DWORD>(PAGE_READONLY));
    std::vector<uint64_t> w;
    ww::take_written(r, w);
    CHECK(w.empty());
    const uint64_t before = ww::faults();
    *reinterpret_cast<volatile uint32_t*>(mem::kHeapBase + 3 * kPg + 8) = 0x1234;   // faults once, then goes through
    CHECK_EQ(*reinterpret_cast<volatile uint32_t*>(mem::kHeapBase + 3 * kPg + 8), 0x1234u);
    CHECK_EQ(ww::faults(), before + 1);
    CHECK(!ww::page_valid(r, 3));
    CHECK(ww::page_valid(r, 2) && ww::page_valid(r, 4) && ww::page_valid(r, 5));
    CHECK_EQ(prot_of(mem::kHeapBase + 3 * kPg), static_cast<DWORD>(PAGE_READWRITE));
    ww::take_written(r, w);
    CHECK(w.size() == 1 && w[0] == 3);
    ww::invalidate(r, 4, 1);                                  // the owner wrote it (GPU): not valid, still read-only
    CHECK(!ww::page_valid(r, 4));
    CHECK_EQ(prot_of(mem::kHeapBase + 4 * kPg), static_cast<DWORD>(PAGE_READONLY));
    CHECK(ww::arm(r, 2, 4));                                  // re-arming a mix of armed and released pages
    CHECK(ww::all_valid(r, 2, 4));
    teardown(r);
}

NXTEST(destroy_makes_pages_writable) {
    ww::Region* r = setup();
    REQUIRE(r != nullptr);
    CHECK(ww::arm(r, 0, kPages));
    ww::region_destroy(r);
    CHECK_EQ(prot_of(mem::kHeapBase), static_cast<DWORD>(PAGE_READWRITE));
    *reinterpret_cast<volatile uint32_t*>(mem::kHeapBase + 10 * kPg) = 7;      // no region: a plain write
    mem::shutdown();
}

NXTEST(file_read_into_armed_pages) {
    ww::Region* r = setup();
    REQUIRE(r != nullptr);
    char tmp[MAX_PATH], path[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    GetTempFileNameA(tmp, "nww", 0, path);
    std::vector<uint8_t> data(3 * kPg);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7);
    FILE* f = std::fopen(path, "wb");
    REQUIRE(f != nullptr);
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING * 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    uint8_t* dst = reinterpret_cast<uint8_t*>(mem::kHeapBase + 8 * kPg);
    CHECK(ww::arm(r, 8, 3));
    DWORD got = 0;
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    CHECK(!ReadFile(h, dst, static_cast<DWORD>(data.size()), &got, nullptr));   // what the scope is for: the kernel cannot write read-only pages
    {
        const ww::OsWriteScope ws(dst, data.size());
        CHECK(!ww::arm(r, 8, 3));                              // no arming while the read is in progress
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        CHECK(ReadFile(h, dst, static_cast<DWORD>(data.size()), &got, nullptr) && got == data.size());
    }
    CHECK(std::memcmp(dst, data.data(), data.size()) == 0);
    CHECK(!ww::page_valid(r, 8));
    CHECK(ww::arm(r, 8, 3));                                   // the scope ended
    CloseHandle(h);
    DeleteFileA(path);
    teardown(r);
}

NXTEST(memory_module_changes_are_respected) {
    ww::Region* r = setup();
    REQUIRE(r != nullptr);
    CHECK(ww::arm(r, 20, 2));
    CHECK(mem::protect(mem::kHeapBase + 20 * kPg, kPg, mem::Prot::R));   // the guest's own protection wins over the tracker's
    CHECK(!ww::page_valid(r, 20));
    CHECK(ww::page_valid(r, 21));
    CHECK_EQ(prot_of(mem::kHeapBase + 20 * kPg), static_cast<DWORD>(PAGE_READONLY));
    CHECK(!ww::arm(r, 20, 1));                                 // a page the guest made read-only is not armed
    CHECK(mem::protect(mem::kHeapBase + 20 * kPg, kPg, mem::Prot::RW));
    CHECK(ww::arm(r, 20, 1));
    CHECK(mem::heap_alias(0x10000, 0x10000) != 0);             // pages 16..31 get a second view: banned
    CHECK(ww::page_banned(r, 20));
    CHECK(!ww::page_valid(r, 20) && !ww::page_valid(r, 21));
    CHECK(!ww::arm(r, 16, 1));
    CHECK_EQ(prot_of(mem::kHeapBase + 21 * kPg), static_cast<DWORD>(PAGE_READWRITE));
    CHECK(ww::arm(r, 2, 1));                                   // pages outside the alias still work
    teardown(r);
}

NXTEST(writer_thread_while_arming) {
    ww::Region* r = setup();
    REQUIRE(r != nullptr);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> writes{0};
    std::thread t([&] {
        uint32_t n = 0;
        while (!stop.load()) {
            for (uint64_t p = 0; p < 16; ++p) *reinterpret_cast<volatile uint32_t*>(mem::kHeapBase + p * kPg + (n % 1024) * 4) = n;
            ++n;
            writes.fetch_add(16);
        }
    });
    int armed = 0;
    for (int i = 0; i < 3000; ++i)
        armed += ww::arm(r, static_cast<uint64_t>(i % 13), 4) ? 1 : 0;
    stop.store(true);
    t.join();
    CHECK(armed > 0);
    CHECK(writes.load() > 0);
    // Invariant: a valid page is read-only; a page that is not armed is writable.
    for (uint64_t p = 0; p < 20; ++p) {
        const DWORD pr = prot_of(mem::kHeapBase + p * kPg);
        if (ww::page_valid(r, p)) CHECK_EQ(pr, static_cast<DWORD>(PAGE_READONLY));
        CHECK(pr == PAGE_READONLY || pr == PAGE_READWRITE);
    }
    for (uint64_t p = 0; p < 16; ++p) *reinterpret_cast<volatile uint32_t*>(mem::kHeapBase + p * kPg) = 1;   // every page still writable through faults
    for (uint64_t p = 0; p < 16; ++p) CHECK(!ww::page_valid(r, p));
    teardown(r);
}
