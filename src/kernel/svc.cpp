// SPDX-License-Identifier: GPL-3.0-or-later
// SVC table and dispatcher (see include/nxrt/svc.h). Dispatch pattern from SuperMarioWonderRecomp runtime/src/kernel.cpp (MIT).
#include "nxrt/svc.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/ftrace.h"
#include "nxrt/log.h"
#include "nxrt/tt.h"

namespace nxrt {
namespace {

struct NameEntry { uint32_t num; const char* name; };
#define NXRT_SVC(n, name) {n, name},
const NameEntry kNames[] = {
#include "svc_names.inc"
};
#undef NXRT_SVC

SvcHandler g_handlers[kSvcCount];
std::atomic<uint64_t> g_calls[kSvcCount];
std::once_flag g_init_once;

std::mutex g_unk_lock;
std::map<uint32_t, SvcUnknownRecord> g_unknown;
std::atomic<uint64_t> g_unknown_total{0};
std::atomic<int64_t> g_log_limit{-1};

// ---- gauges (include/nxrt/gauge.h) ----
// Removes its gauge when the file's statics are torn down (declared after the tables above, so it goes first).
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { gauge_register(n, std::move(f)); }
    ~GaugeHook() { gauge_unregister(name); }
};
GaugeHook gauge_svc_calls("kernel.svc_calls", [] {   // SVCs answered since start (all numbers)
    uint64_t n = 0;
    for (auto& c : g_calls) n += c.load(std::memory_order_relaxed);
    return static_cast<int64_t>(n);
});
GaugeHook gauge_svc_unknown_kinds("kernel.svc_unknown_kinds", [] {   // distinct unknown SVC numbers seen (the map only grows)
    std::lock_guard<std::mutex> lk(g_unk_lock);
    return static_cast<int64_t>(g_unknown.size());
});

}  // namespace

const char* svc_name(uint32_t number) {
    for (auto& e : kNames)
        if (e.num == number) return e.name;
    return "?";
}

void svc_register(uint32_t number, SvcHandler fn) {
    if (number < kSvcCount) g_handlers[number] = fn;
}

SvcHandler svc_handler(uint32_t number) { return number < kSvcCount ? g_handlers[number] : nullptr; }

void svc_init() {
    std::call_once(g_init_once, [] {
        svc_register_memory_group();
        svc_register_info_group();
        svc_register_thread_group();
        svc_register_ipc_group();
        size_t n = 0;
        for (auto* h : g_handlers) n += h != nullptr;
        NXLOG_I("svc", "%zu SVC handlers registered", n);
    });
}

static void report_unknown(::Ctx* c, uint32_t number, uint64_t pc) {
    uint64_t count;
    {
        std::lock_guard<std::mutex> lk(g_unk_lock);
        SvcUnknownRecord& r = g_unknown[number];
        if (r.count == 0) {
            r.number = number;
            for (int i = 0; i < 8; i++) r.first_args[i] = c->x[i];
            r.first_lr = c->x[30];
            r.first_fn = c->fn;
            r.first_pc = pc;
        }
        count = ++r.count;
    }
    g_unknown_total.fetch_add(1, std::memory_order_relaxed);
    if (g_log_limit.load() < 0) g_log_limit = config_get_int("svc", "unknown_log_limit", 100);
    if (static_cast<int64_t>(count) <= g_log_limit.load() || count % 1000 == 0) {
        const kernel::KThread* t = kernel::current_thread();
        NXLOG_W("svc", "UNKNOWN SVC 0x%02X (%s) #%llu x0=%llx x1=%llx x2=%llx x3=%llx x4=%llx x5=%llx x6=%llx x7=%llx caller: lr=%llx pc=%llx in f_%llx thread %s",
                number, svc_name(number), static_cast<unsigned long long>(count), (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->x[4], (unsigned long long)c->x[5],
                (unsigned long long)c->x[6], (unsigned long long)c->x[7], (unsigned long long)c->x[30], (unsigned long long)pc,
                (unsigned long long)c->fn, t ? t->name.c_str() : "?");
    }
}

void svc_dispatch(::Ctx* c, uint32_t number, uint64_t pc) {
    if (tt::detail::svc_every.load(std::memory_order_relaxed)) tt::detail::on_svc(c);      // test tooling: frame_source = svc:N
    SvcHandler h = svc_handler(number);
    if (number < kSvcCount) g_calls[number].fetch_add(1, std::memory_order_relaxed);
    if (!h) {
        report_unknown(c, number, pc);
        c->x[0] = kernel::kNotImplemented;
        return;
    }
    if (ft::on()) {
        const uint64_t t0 = ft::now_ns();
        h(c);
        const uint64_t t1 = ft::now_ns();
        if (t1 - t0 >= 100000) { static const uint16_t z = ft::zone_id("svc"); ft::event(z, t0, t1, number); }
        return;
    }
    h(c);
}

std::vector<SvcUnknownRecord> svc_unknown_records() {
    std::lock_guard<std::mutex> lk(g_unk_lock);
    std::vector<SvcUnknownRecord> v;
    for (auto& kv : g_unknown) v.push_back(kv.second);
    return v;
}

uint64_t svc_unknown_total() { return g_unknown_total.load(); }
uint64_t svc_call_count(uint32_t number) { return number < kSvcCount ? g_calls[number].load() : 0; }

void svc_reset_counters() {
    std::lock_guard<std::mutex> lk(g_unk_lock);
    g_unknown.clear();
    g_unknown_total = 0;
    g_log_limit = -1;
    for (auto& c : g_calls) c = 0;
}

std::string svc_summary() {
    uint64_t total = 0;
    size_t distinct = 0;
    for (auto& c : g_calls)
        if (c.load()) { total += c.load(); distinct++; }
    std::string s = "svc: " + std::to_string(total) + " calls, " + std::to_string(distinct) + " distinct, " + std::to_string(g_unknown_total.load()) + " unknown";
    std::lock_guard<std::mutex> lk(g_unk_lock);
    for (auto& kv : g_unknown) {
        char b[96];
        snprintf(b, sizeof b, " [0x%02X %s x%llu]", kv.first, svc_name(kv.first), static_cast<unsigned long long>(kv.second.count));
        s += b;
    }
    return s;
}

}  // namespace nxrt
