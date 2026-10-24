// SPDX-License-Identifier: GPL-3.0-or-later
// Gauges for long-session monitoring (S34). A gauge is a named integer that a subsystem can report on request: the number of live
// textures, the size of a cache, bytes held by a pool. The soak sampler (`[diag] soak_csv`) reads all gauges every few seconds and writes
// one JSON line per sample; nothing runs when the key is empty, and registering a gauge costs one map insert.
//
//   static nxrt::GaugeCounter g_images("vk.images");       // a live-object counter: ++ at create, -- at destroy (relaxed atomics)
//   nxrt::gauge_register("nvn.pipelines", [] { return (int64_t)g_pipelines.size(); });   // a callback, called from the sampler thread
//
// A callback runs on the sampler thread while the game runs: it must take the lock that protects what it reads, or read an atomic. A
// callback that cannot be made safe should not be registered. Names are "area.what" in lower case, unique; registering a name again
// replaces the callback. gauge_unregister removes it (call it before the object the callback reads is destroyed).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace nxrt {

void gauge_register(const char* name, std::function<int64_t()> fn);
void gauge_unregister(const char* name);
// Every gauge as (name, value), sorted by name. Callbacks are called with the registry lock held, so they must not register gauges.
std::vector<std::pair<std::string, int64_t>> gauge_read_all();

// A live-object counter that registers itself on construction (use a namespace-scope or function-local static).
class GaugeCounter {
public:
    explicit GaugeCounter(const char* name);
    ~GaugeCounter();
    GaugeCounter(const GaugeCounter&) = delete;
    GaugeCounter& operator=(const GaugeCounter&) = delete;
    void add(int64_t n = 1) { v_.fetch_add(n, std::memory_order_relaxed); }
    void sub(int64_t n = 1) { v_.fetch_sub(n, std::memory_order_relaxed); }
    GaugeCounter& operator++() { add(1); return *this; }
    GaugeCounter& operator--() { sub(1); return *this; }
    int64_t get() const { return v_.load(std::memory_order_relaxed); }
private:
    std::string name_;
    std::atomic<int64_t> v_{0};
};

// ---- the sampler -----------------------------------------------------------------------------------------------------------
// Started by the runtime when `[diag] soak_csv` (env NXRT_DIAG_SOAK_CSV) names a file; `[diag] soak_interval_s` (default 10) is the period.
// Each line is a JSON object: t (seconds since start), frame, win_frames, ft_p50/ft_p99/ft_max (ms between presented frames in the window),
// process figures (ws, private, peak_ws, pagefile, handles, threads, gdi, user, cpu_ms), then every gauge by name.
void soak_start();
void soak_stop();
// Called by diag_frame_tick(): records the time of a presented frame for the window statistics. Cheap (two relaxed stores).
void soak_note_frame();

}  // namespace nxrt
