// SPDX-License-Identifier: GPL-3.0-or-later
// S38: page write tracking (include/nxrt/write_watch.h).
#ifdef _WIN32
#include <windows.h>     // only the file-read case below (until nxrt/os/fs.h exists)
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/memory.h"
#include "nxrt/os/fault.h"
#include "nxrt/os/mem.h"
#include "nxrt/write_watch.h"
#include "test_framework.h"

NXTEST_GROUP(write_watch)

namespace {

namespace mem = nxrt::mem;
namespace ww = nxrt::mem::ww;
constexpr uint64_t kPg = 4096;
constexpr uint64_t kHeap = 64ull << 20;
constexpr uint64_t kPages = 64;

namespace osm = nxrt::os::mem;
using nxrt::os::mem::Prot;

Prot prot_of(uint64_t addr) {
    osm::Region r;
    osm::query(addr, &r, kPg);
    return r.state == osm::State::Committed ? r.prot : Prot::None;
}

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
    CHECK_EQ(prot_of(mem::kHeapBase + 3 * kPg), Prot::R);
    std::vector<uint64_t> w;
    ww::take_written(r, w);
    CHECK(w.empty());
    const uint64_t before = ww::faults();
    *reinterpret_cast<volatile uint32_t*>(mem::kHeapBase + 3 * kPg + 8) = 0x1234;   // faults once, then goes through
    CHECK_EQ(*reinterpret_cast<volatile uint32_t*>(mem::kHeapBase + 3 * kPg + 8), 0x1234u);
    CHECK_EQ(ww::faults(), before + 1);
    CHECK(!ww::page_valid(r, 3));
    CHECK(ww::page_valid(r, 2) && ww::page_valid(r, 4) && ww::page_valid(r, 5));
    CHECK_EQ(prot_of(mem::kHeapBase + 3 * kPg), Prot::RW);
    ww::take_written(r, w);
    CHECK(w.size() == 1 && w[0] == 3);
    ww::invalidate(r, 4, 1);                                  // the owner wrote it (GPU): not valid, still read-only
    CHECK(!ww::page_valid(r, 4));
    CHECK_EQ(prot_of(mem::kHeapBase + 4 * kPg), Prot::R);
    CHECK(ww::arm(r, 2, 4));                                  // re-arming a mix of armed and released pages
    CHECK(ww::all_valid(r, 2, 4));
    teardown(r);
}

NXTEST(destroy_makes_pages_writable) {
    ww::Region* r = setup();
    REQUIRE(r != nullptr);
    CHECK(ww::arm(r, 0, kPages));
    ww::region_destroy(r);
    CHECK_EQ(prot_of(mem::kHeapBase), Prot::RW);
    *reinterpret_cast<volatile uint32_t*>(mem::kHeapBase + 10 * kPg) = 7;      // no region: a plain write
    mem::shutdown();
}

NXTEST(file_read_into_armed_pages) {
    ww::Region* r = setup();
    REQUIRE(r != nullptr);
    std::vector<uint8_t> data(3 * kPg);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7);
#ifdef _WIN32
    char tmp[MAX_PATH], path[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    GetTempFileNameA(tmp, "nww", 0, path);
#else
    char path[] = "/tmp/nwwXXXXXX";
    { const int tfd = mkstemp(path); REQUIRE(tfd >= 0); close(tfd); }
#endif
    FILE* f = std::fopen(path, "wb");
    REQUIRE(f != nullptr);
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    uint8_t* dst = reinterpret_cast<uint8_t*>(mem::kHeapBase + 8 * kPg);
    CHECK(ww::arm(r, 8, 3));
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    auto os_read = [&] {
        DWORD got = 0;
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        return ReadFile(h, dst, static_cast<DWORD>(data.size()), &got, nullptr) && got == data.size();
    };
#else
    const int h = open(path, O_RDONLY);
    REQUIRE(h >= 0);
    auto os_read = [&] { return pread(h, dst, data.size(), 0) == static_cast<ssize_t>(data.size()); };
#endif
    CHECK(!os_read());                                         // what the scope is for: the kernel cannot write read-only pages
    {
        const ww::OsWriteScope ws(dst, data.size());
        CHECK(!ww::arm(r, 8, 3));                              // no arming while the read is in progress
        CHECK(os_read());
    }
    CHECK(std::memcmp(dst, data.data(), data.size()) == 0);
    CHECK(!ww::page_valid(r, 8));
    CHECK(ww::arm(r, 8, 3));                                   // the scope ended
#ifdef _WIN32
    CloseHandle(h);
    DeleteFileA(path);
#else
    close(h);
    unlink(path);
#endif
    teardown(r);
}

NXTEST(memory_module_changes_are_respected) {
    ww::Region* r = setup();
    REQUIRE(r != nullptr);
    CHECK(ww::arm(r, 20, 2));
    CHECK(mem::protect(mem::kHeapBase + 20 * kPg, kPg, mem::Prot::R));   // the guest's own protection wins over the tracker's
    CHECK(!ww::page_valid(r, 20));
    CHECK(ww::page_valid(r, 21));
    CHECK_EQ(prot_of(mem::kHeapBase + 20 * kPg), Prot::R);
    CHECK(!ww::arm(r, 20, 1));                                 // a page the guest made read-only is not armed
    CHECK(mem::protect(mem::kHeapBase + 20 * kPg, kPg, mem::Prot::RW));
    CHECK(ww::arm(r, 20, 1));
    CHECK(mem::heap_alias(0x10000, 0x10000) != 0);             // pages 16..31 get a second view: banned
    CHECK(ww::page_banned(r, 20));
    CHECK(!ww::page_valid(r, 20) && !ww::page_valid(r, 21));
    CHECK(!ww::arm(r, 16, 1));
    CHECK_EQ(prot_of(mem::kHeapBase + 21 * kPg), Prot::RW);
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
    for (int i = 0; i < 5000 && writes.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));     // the writer may start late on a loaded machine
    stop.store(true);
    t.join();
    CHECK(armed > 0);
    CHECK(writes.load() > 0);
    // Invariant: a valid page is read-only; a page that is not armed is writable.
    for (uint64_t p = 0; p < 20; ++p) {
        const Prot pr = prot_of(mem::kHeapBase + p * kPg);
        if (ww::page_valid(r, p)) CHECK_EQ(pr, Prot::R);
        CHECK(pr == Prot::R || pr == Prot::RW);
    }
    for (uint64_t p = 0; p < 16; ++p) *reinterpret_cast<volatile uint32_t*>(mem::kHeapBase + p * kPg) = 1;   // every page still writable through faults
    for (uint64_t p = 0; p < 16; ++p) CHECK(!ww::page_valid(r, p));
    teardown(r);
}

namespace {

// A write that may fault; true when the fault went past the tracker to the guard (the caller's own handler).
bool write_passes_or_faults(uint64_t addr) {
    const uint32_t v = 0x55aa;
    return !nxrt::os::guard::write(reinterpret_cast<void*>(addr), &v, sizeof v);
}

}  // namespace

NXTEST(foreign_fault_passes_on_at_once) {
    ww::Region* r = setup();
    REQUIRE(r != nullptr);
    REQUIRE(osm::protect(mem::kHeapBase + 30 * kPg, kPg, Prot::R));   // inside the region, not armed by us
    const uint64_t faults0 = ww::faults(), waits0 = ww::fault_waits();
    CHECK(write_passes_or_faults(mem::kHeapBase + 30 * kPg + 16));          // nobody is releasing: not ours, no waiting
    CHECK(write_passes_or_faults(mem::kHeapBase + 30 * kPg + 32));
    CHECK_EQ(ww::fault_waits(), waits0);
    CHECK_EQ(ww::faults(), faults0);
    CHECK_EQ(prot_of(mem::kHeapBase + 30 * kPg), Prot::R);
    // an armed page next to it is still handled
    CHECK(ww::arm(r, 31, 1));
    CHECK(!write_passes_or_faults(mem::kHeapBase + 31 * kPg));
    CHECK_EQ(ww::faults(), faults0 + 1);
    // after a release has finished, a second fault on the same page (a late writer) finds it writable and goes on without waiting
    CHECK(!write_passes_or_faults(mem::kHeapBase + 31 * kPg + 4));
    teardown(r);
}

NXTEST(writers_race_for_the_same_armed_pages) {
    ww::Region* r = setup();
    REQUIRE(r != nullptr);
    std::atomic<bool> stop{false};
    std::atomic<int> failed{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&, t] {
            uint32_t n = static_cast<uint32_t>(t);
            while (!stop.load()) {
                for (uint64_t p = 0; p < 8; ++p)
                    if (write_passes_or_faults(mem::kHeapBase + p * kPg + (n % 1024) * 4)) failed.fetch_add(1);   // a fault the tracker did not handle
                ++n;
            }
        });
    // keep arming until the writers have really raced the arms (they start late on a loaded machine): at least 3000 arms and 20000 faults, or 5 s
    const uint64_t faults0 = ww::faults();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 3000 || (ww::faults() - faults0 < 20000 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)); ++i) ww::arm(r, 0, 8);
    stop.store(true);
    for (auto& t : ts) t.join();
    CHECK_EQ(failed.load(), 0);
    CHECK(ww::faults() > faults0);
    teardown(r);
}
