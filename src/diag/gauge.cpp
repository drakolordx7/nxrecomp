// SPDX-License-Identifier: GPL-3.0-or-later
// Gauges and the soak sampler (declared in nxrt/gauge.h).
#include "nxrt/gauge.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/log.h"

namespace nxrt {
namespace {

struct Registry {
    std::mutex m;
    std::map<std::string, std::function<int64_t()>> g;
};
Registry& reg() {
    static Registry* r = new Registry;     // never destroyed: counters in other statics may unregister at exit
    return *r;
}

// ---- frame times of the window -------------------------------------------------------------------------------------------
constexpr size_t kRing = 1 << 16;           // frames kept between two samples (enough for 6,500 fps at a 10 s window)
std::atomic<uint64_t> g_ring_n{0};
std::atomic<uint64_t> g_ring[kRing];        // QueryPerformanceCounter ticks of each presented frame

uint64_t qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return (uint64_t)v.QuadPart;
}
double qpc_freq() {
    static double f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return (double)v.QuadPart; }();
    return f;
}

struct Sampler {
    std::mutex ctl;
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool stop = false;
    FILE* f = nullptr;
};
Sampler& smp() {
    static Sampler* s = new Sampler;
    return *s;
}

uint32_t thread_count() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    uint32_t n = 0;
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) do { if (te.th32OwnerProcessID == pid) n++; } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    return n;
}

void run(double interval_s, double t0) {
    Sampler& s = smp();
    uint64_t last_n = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(s.m);
            if (s.cv.wait_for(lk, std::chrono::duration<double>(interval_s), [&] { return s.stop; })) return;
        }
        const double now = (double)qpc() / qpc_freq();
        const uint64_t n = g_ring_n.load(std::memory_order_acquire);
        std::vector<double> dt;
        const uint64_t first = (n > last_n + kRing) ? n - kRing : last_n;
        uint64_t prev = first > 0 ? g_ring[(first - 1) % kRing].load(std::memory_order_relaxed) : 0;
        for (uint64_t i = first; i < n; i++) {
            const uint64_t t = g_ring[i % kRing].load(std::memory_order_relaxed);
            if (prev) dt.push_back((double)(t - prev) * 1000.0 / qpc_freq());
            prev = t;
        }
        const uint64_t window_frames = n - last_n;
        last_n = n;
        double p50 = 0, p99 = 0, mx = 0;
        if (!dt.empty()) {
            std::sort(dt.begin(), dt.end());
            p50 = dt[dt.size() / 2];
            p99 = dt[std::min(dt.size() - 1, (size_t)(dt.size() * 0.99))];
            mx = dt.back();
        }
        PROCESS_MEMORY_COUNTERS_EX pm{};
        pm.cb = sizeof pm;
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        const double cpu_ms = (((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10000.0;
        char buf[640];
        snprintf(buf, sizeof buf,
                 "{\"t\":%.1f,\"frame\":%llu,\"win_frames\":%llu,\"ft_p50\":%.3f,\"ft_p99\":%.3f,\"ft_max\":%.2f,\"ws\":%llu,\"private\":%llu,\"peak_ws\":%llu,"
                 "\"pagefile\":%llu,\"handles\":%lu,\"threads\":%u,\"gdi\":%lu,\"user\":%lu,\"cpu_ms\":%.0f",
                 now - t0, (unsigned long long)diag_frame_count(), (unsigned long long)window_frames, p50, p99, mx, (unsigned long long)pm.WorkingSetSize,
                 (unsigned long long)pm.PrivateUsage, (unsigned long long)pm.PeakWorkingSetSize, (unsigned long long)pm.PagefileUsage, (unsigned long)handles,
                 thread_count(), (unsigned long)GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), (unsigned long)GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS),
                 cpu_ms);
        std::string line = buf;
        for (auto& kv : gauge_read_all()) {
            snprintf(buf, sizeof buf, ",\"%s\":%lld", kv.first.c_str(), (long long)kv.second);
            line += buf;
        }
        line += "}\n";
        fwrite(line.data(), 1, line.size(), s.f);
        fflush(s.f);
    }
}

}  // namespace

void gauge_register(const char* name, std::function<int64_t()> fn) {
    Registry& r = reg();
    std::lock_guard<std::mutex> lk(r.m);
    r.g[name] = std::move(fn);
}

void gauge_unregister(const char* name) {
    Registry& r = reg();
    std::lock_guard<std::mutex> lk(r.m);
    r.g.erase(name);
}

std::vector<std::pair<std::string, int64_t>> gauge_read_all() {
    Registry& r = reg();
    std::lock_guard<std::mutex> lk(r.m);
    std::vector<std::pair<std::string, int64_t>> out;
    out.reserve(r.g.size());
    for (auto& kv : r.g) out.emplace_back(kv.first, kv.second());
    return out;
}

GaugeCounter::GaugeCounter(const char* name) : name_(name) {
    gauge_register(name, [this] { return get(); });
}
GaugeCounter::~GaugeCounter() { gauge_unregister(name_.c_str()); }

void soak_note_frame() {
    const uint64_t n = g_ring_n.load(std::memory_order_relaxed);
    g_ring[n % kRing].store(qpc(), std::memory_order_relaxed);
    g_ring_n.store(n + 1, std::memory_order_release);
}

void soak_start() {
    const std::string path = config_get_str("diag", "soak_csv", "");
    if (path.empty()) return;
    Sampler& s = smp();
    std::lock_guard<std::mutex> g(s.ctl);
    if (s.th.joinable()) return;
    s.f = fopen(path.c_str(), "ab");
    if (!s.f) {
        NXLOG_W("diag", "soak: cannot open %s", path.c_str());
        return;
    }
    const double iv = std::max(0.5, config_get_double("diag", "soak_interval_s", 10.0));
    s.stop = false;
    const double t0 = (double)qpc() / qpc_freq();
    s.th = std::thread(run, iv, t0);
    NXLOG_I("diag", "soak sampler: %s every %.1f s", path.c_str(), iv);
}

void soak_stop() {
    Sampler& s = smp();
    std::lock_guard<std::mutex> g(s.ctl);
    if (!s.th.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.stop = true;
    }
    s.cv.notify_all();
    s.th.join();
    if (s.f) { fclose(s.f); s.f = nullptr; }
}

}  // namespace nxrt
