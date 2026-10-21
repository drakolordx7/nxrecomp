// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from SuperMarioWonderRecomp runtime/src/memguard_win.cpp (MIT, Copyright (c) 2026 BelmanteGu); see THIRD_PARTY_NOTICES.md.
#include "nxrt/memguard.h"

#include "nxrt/config.h"
#include "nxrt/log.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <thread>

namespace nxrt {
namespace {

using Clock = std::chrono::steady_clock;

struct Settings {
    uint64_t limit;
    uint32_t poll_ms;
    void (*on_exceed)(uint64_t footprint, uint64_t limit, void* user);
    void* user;
};

std::mutex g_ctl;             // serialises start / stop / running
std::mutex g_mu;              // guards g_stop and the poll wait
std::condition_variable g_cv;
bool g_stop = false;          // guarded by g_mu
std::thread g_thread;         // guarded by g_ctl
std::atomic<uint64_t> g_peak{0};
std::atomic<bool> g_tripped{false};

constexpr double kBytesPerGb = 1073741824.0;

uint64_t gb_to_bytes(double gb) {
    const double b = gb * kBytesPerGb;
    if (b >= static_cast<double>(std::numeric_limits<uint64_t>::max())) return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(b);
}

void raise_peak(uint64_t f) {
    uint64_t p = g_peak.load();
    while (f > p && !g_peak.compare_exchange_weak(p, f)) {
    }
}

void guard_loop(Settings s) {
    const double limit_gb = static_cast<double>(s.limit) / kBytesPerGb;
    auto next_log = Clock::now() + std::chrono::seconds(30);
    std::unique_lock<std::mutex> lk(g_mu);
    for (;;) {
        // Returns true as soon as g_stop is set (memguard_stop notifies), so stop is prompt.
        if (g_cv.wait_for(lk, std::chrono::milliseconds(s.poll_ms), [] { return g_stop; })) return;
        lk.unlock();

        const uint64_t f = memguard_footprint();
        raise_peak(f);
        const double gb = static_cast<double>(f) / kBytesPerGb;

        if (Clock::now() >= next_log) {
            next_log = Clock::now() + std::chrono::seconds(30);
            NXLOG_I("memory", "footprint %.2f GB (peak %.2f GB, limit %.1f GB)", gb,
                    static_cast<double>(g_peak.load()) / kBytesPerGb, limit_gb);
        }

        if (f > s.limit && !g_tripped.exchange(true)) {
            if (s.on_exceed) {
                s.on_exceed(f, s.limit, s.user);
            } else {
                NXLOG_E("memory", "%.2f GB is over the %.1f GB limit ([memory] limit_gb): stopping", gb, limit_gb);
                log_flush();
                std::fprintf(stderr, "memory: %.2f GB is over the %.1f GB limit: stopping\n", gb, limit_gb);
                std::fflush(nullptr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }

        lk.lock();
    }
}

}  // namespace

uint64_t memguard_footprint() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc))
        return 0;
    return static_cast<uint64_t>(pmc.PrivateUsage);
}

double memguard_limit_gb(const char* env_text) {
    const double gb = env_text ? std::atof(env_text) : 0.0;
    return (gb > 0.0 && std::isfinite(gb)) ? gb : 8.0;
}

void memguard_start(const MemGuardOptions& opt) {
    std::lock_guard<std::mutex> ctl(g_ctl);
    if (g_thread.joinable()) return;  // already running: ignored

    Settings s{};
    s.limit = opt.limit_bytes ? opt.limit_bytes : gb_to_bytes(memguard_limit_gb(config_get_str("memory", "limit_gb", "8").c_str()));
    const int64_t poll = config_get_int("memory", "poll_ms", 250);
    s.poll_ms = opt.poll_ms ? opt.poll_ms : (poll > 0 ? static_cast<uint32_t>(poll) : 250u);
    s.on_exceed = opt.on_exceed;
    s.user = opt.user;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = false;
    }
    g_peak.store(0);
    g_tripped.store(false);
    g_thread = std::thread(guard_loop, s);
}

void memguard_stop() {
    std::lock_guard<std::mutex> ctl(g_ctl);
    if (!g_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = true;
    }
    g_cv.notify_all();
    g_thread.join();
}

bool memguard_running() {
    std::lock_guard<std::mutex> ctl(g_ctl);
    return g_thread.joinable();
}

uint64_t memguard_peak() { return g_peak.load(); }

bool memguard_tripped() { return g_tripped.load(); }

}  // namespace nxrt
