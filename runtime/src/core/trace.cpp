// SPDX-License-Identifier: GPL-3.0-or-later
// Guest tracer (nxrt/trace.h).
#include "nxrt/trace.h"

#include <atomic>
#include <cstdlib>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::trace {
namespace {

constexpr uint64_t kMain = mem::kMainBase;
constexpr uint64_t kMainSpan = mem::kHleBase - mem::kMainBase;
std::atomic<EntryDump> g_dump{nullptr};

using ull = unsigned long long;

uint64_t rd(uint64_t a) { return *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(a)); }

std::vector<uint64_t> parse(const std::string& list) {
    std::vector<uint64_t> out;
    for (size_t p = 0; p < list.size();) {
        size_t e = list.find(',', p);
        if (e == std::string::npos) e = list.size();
        if (e > p) {
            const uint64_t v = std::strtoull(list.substr(p, e - p).c_str(), nullptr, 16);
            out.push_back(v >= kMain ? v : kMain + v);
        }
        p = e + 1;
    }
    return out;
}

int on_entry(::Ctx* c, uint64_t addr, void*) {
    NXLOG_I("trace", "enter main+%llx x0=%llx x1=%llx x2=%llx x3=%llx lr=main+%llx", (ull)(addr - kMain), (ull)c->x[0], (ull)c->x[1],
            (ull)c->x[2], (ull)c->x[3], (ull)(c->x[30] - kMain));
    if (mem::guest_readable(c->x[0], 8)) {
        const uint64_t vt = rd(c->x[0]);
        if (vt >= kMain && vt - kMain < kMainSpan && mem::guest_readable(vt, 48)) {
            const uint64_t* v = reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(vt));
            NXLOG_I("trace", "  [x0] vtable main+%llx slots main+%llx %llx %llx %llx %llx %llx", (ull)(vt - kMain), (ull)(v[0] - kMain),
                    (ull)(v[1] - kMain), (ull)(v[2] - kMain), (ull)(v[3] - kMain), (ull)(v[4] - kMain), (ull)(v[5] - kMain));
        }
    }
    if (mem::guest_readable(c->x[1], 80)) {
        const uint64_t* m = reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(c->x[1]));
        NXLOG_I("trace", "  [x1] %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx", (ull)m[0], (ull)m[1], (ull)m[2], (ull)m[3], (ull)m[4],
                (ull)m[5], (ull)m[6], (ull)m[7], (ull)m[8], (ull)m[9]);
    }
    if (const EntryDump d = g_dump.load(std::memory_order_acquire)) d(c, addr);
    return 0;
}

void on_at(::Ctx* c, uint64_t addr, void*) {
    NXLOG_I("trace", "at main+%llx x0=%llx x1=%llx x2=%llx x3=%llx x19..x28=%llx %llx %llx %llx %llx %llx %llx %llx %llx %llx", (ull)(addr - kMain),
            (ull)c->x[0], (ull)c->x[1], (ull)c->x[2], (ull)c->x[3], (ull)c->x[19], (ull)c->x[20], (ull)c->x[21], (ull)c->x[22], (ull)c->x[23],
            (ull)c->x[24], (ull)c->x[25], (ull)c->x[26], (ull)c->x[27], (ull)c->x[28]);
}

}  // namespace

void trace_fns(const std::string& list) {
    for (uint64_t a : parse(list)) hook_add_entry(a, on_entry);
}

void trace_at(const std::string& list) {
    for (uint64_t a : parse(list)) hook_add_inline(a, on_at);
}

void set_entry_dump(EntryDump fn) { g_dump.store(fn, std::memory_order_release); }

void install_from_config() {
    trace_fns(config_get_str("trace", "fns", ""));
    trace_at(config_get_str("trace", "at", ""));
}

}  // namespace nxrt::trace
