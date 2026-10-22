// SPDX-License-Identifier: GPL-3.0-or-later
// Low-latency modes (include/nxrt/lowlat.h): NVIDIA Reflex (VK_NV_low_latency2) and AMD Anti-Lag 2 (VK_AMD_anti_lag).
#include "nxrt/lowlat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/log.h"

namespace nxrt::extras {
namespace {
constexpr const char* kTag = "lowlat";
std::atomic<LowLatency*> g_instance{nullptr};

uint64_t now_us() { return FrameLimiter::now_ns() / 1000ull; }
double ms_since(uint64_t t_ns) { return static_cast<double>(FrameLimiter::now_ns() - t_ns) * 1e-6; }
}  // namespace

struct LowLatency::Impl {
    gfx::Device* dev = nullptr;
    LatencySettings cfg;
    VkSwapchainLatencyCreateInfoNV sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV};

    // swapchain and sleep mode
    std::mutex mu;
    std::condition_variable cv;               // notified by present_begin: a frame left the in-flight queue
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    bool mode_set = false;
    double cap_fps = 0;
    VkSemaphore sleep_sem = VK_NULL_HANDLE;
    uint64_t sleep_value = 0;

    // frame ids: sim_begin takes the next, sim_end queues it, present_begin pops the oldest
    std::atomic<uint64_t> sim_id{0};
    std::deque<uint64_t> pending;            // ids simulated and not yet presented (guarded by mu)
    uint64_t present_id = 0;                  // id of the present in progress (render thread)
    uint64_t last_present_id = 0;
    bool submit_marked = false;               // RENDERSUBMIT_START was set for the frame in progress

    // measurement
    std::atomic<uint64_t> input_ns_for[64] = {};   // id % 64 -> time of INPUT_SAMPLE
    std::atomic<uint64_t> frames{0};
    double sleep_ms_avg = 0, in2present_ema = 0, in2gpu_ema = 0, sim2present_ema = 0;
    std::atomic<uint64_t> reports{0};
    uint64_t last_report_us = 0, last_log_ns = 0;
    uint64_t report_seen_id = 0;
    bool have_ema = false, have_gpu_ema = false;

    void set_mode_locked() {
        if (!swapchain) return;
        if (backend_reflex()) {
            VkLatencySleepModeInfoNV mi = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
            mi.lowLatencyMode = VK_TRUE;
            mi.lowLatencyBoost = cfg.mode == LatencyMode::Boost ? VK_TRUE : VK_FALSE;
            mi.minimumIntervalUs = cap_fps > 0 ? static_cast<uint32_t>(1e6 / cap_fps) : 0;
            const VkResult r = vkSetLatencySleepModeNV(dev->device(), swapchain, &mi);
            mode_set = r == VK_SUCCESS;
            if (!mode_set) NXLOG_W(kTag, "vkSetLatencySleepModeNV failed (%d)", static_cast<int>(r));
            else NXLOG_I(kTag, "Reflex sleep mode set: low latency on, boost %s, interval %u us", mi.lowLatencyBoost ? "on" : "off", mi.minimumIntervalUs);
        }
    }
    bool backend_reflex() const { return reflex; }
    bool reflex = false, antilag = false;
    bool paced = false;                       // the mode is on: the frames_ahead wait runs (also on GPUs with neither extension)
    bool active = false;                      // markers and the host measurement run (enabled, or log_timings asks for the numbers)

    void marker(VkLatencyMarkerNV m, uint64_t id) {
        if (!reflex || !swapchain || !mode_set) return;
        VkSetLatencyMarkerInfoNV mi = {VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV};
        mi.presentID = id;
        mi.marker = m;
        vkSetLatencyMarkerNV(dev->device(), swapchain, &mi);
    }

    void antilag_update(VkAntiLagStageAMD stage, uint64_t id) {
        VkAntiLagPresentationInfoAMD pi = {VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD};
        pi.stage = stage;
        pi.frameIndex = id;
        VkAntiLagDataAMD d = {VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD};
        d.mode = VK_ANTI_LAG_MODE_ON_AMD;
        d.maxFPS = cap_fps > 0 ? static_cast<uint32_t>(cap_fps) : 0;
        d.pPresentationInfo = &pi;
        vkAntiLagUpdateAMD(dev->device(), &d);
    }

    void poll_reports() {
        if (!reflex || !swapchain || !mode_set) return;
        const uint64_t t = now_us();
        if (t - last_report_us < 200000) return;       // five polls a second are plenty
        last_report_us = t;
        VkGetLatencyMarkerInfoNV gi = {VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV};
        gi.timingCount = 0;
        gi.pTimings = nullptr;
        vkGetLatencyTimingsNV(dev->device(), swapchain, &gi);
        if (gi.timingCount == 0) return;
        std::vector<VkLatencyTimingsFrameReportNV> v(gi.timingCount);
        for (auto& f : v) f = {VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV};
        gi.pTimings = v.data();
        vkGetLatencyTimingsNV(dev->device(), swapchain, &gi);
        double sum_gpu = 0, sum_sp = 0;
        uint32_t n = 0;
        for (uint32_t i = 0; i < gi.timingCount; ++i) {
            const auto& f = v[i];
            if (f.presentID <= report_seen_id) continue;
            if (!f.inputSampleTimeUs || !f.gpuRenderEndTimeUs || f.gpuRenderEndTimeUs < f.inputSampleTimeUs) continue;
            sum_gpu += static_cast<double>(f.gpuRenderEndTimeUs - f.inputSampleTimeUs) * 1e-3;
            if (f.simStartTimeUs && f.presentEndTimeUs >= f.simStartTimeUs) sum_sp += static_cast<double>(f.presentEndTimeUs - f.simStartTimeUs) * 1e-3;
            report_seen_id = std::max(report_seen_id, f.presentID);
            ++n;
        }
        if (n) {
            const double g = sum_gpu / n, sp = sum_sp / n;
            in2gpu_ema = have_gpu_ema ? in2gpu_ema * 0.9 + g * 0.1 : g;
            sim2present_ema = have_gpu_ema ? sim2present_ema * 0.9 + sp * 0.1 : sp;
            have_gpu_ema = true;
            reports += n;
        }
    }
};

LowLatency* lowlat() { return g_instance.load(std::memory_order_acquire); }
void lowlat_set(LowLatency* l) { g_instance.store(l, std::memory_order_release); }

LowLatency::LowLatency(gfx::Device& dev) : impl_(new Impl) {
    Impl& s = *impl_;
    s.dev = &dev;
    s.cfg = settings().latency;
    const gfx::DeviceFeatures& f = dev.features();
    if (s.cfg.mode != LatencyMode::Off) {
        if (f.low_latency2) { s.reflex = true; backend_ = Backend::Reflex; }
        else if (f.anti_lag) { s.antilag = true; backend_ = Backend::AntiLag; }
        else NXLOG_W(kTag, "low latency requested, but the GPU has neither VK_NV_low_latency2 (with present id) nor VK_AMD_anti_lag: off");
    }
    // Boost is a Reflex setting; with Anti-Lag it behaves as On. Auto and On both use whichever backend exists.
    enabled_ = backend_ != Backend::None;
    s.paced = s.cfg.mode != LatencyMode::Off;
    s.active = enabled_ || s.paced || s.cfg.log_timings;
    if (enabled_) {
        s.sci.latencyModeEnable = s.reflex ? VK_TRUE : VK_FALSE;
        if (s.reflex) {
            VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
            if (vkCreateSemaphore(dev.device(), &sci, nullptr, &s.sleep_sem) != VK_SUCCESS) {
                NXLOG_W(kTag, "cannot create the Reflex sleep semaphore: off");
                s.reflex = false;
                enabled_ = false;
                s.active = s.paced || s.cfg.log_timings;
                backend_ = Backend::None;
            }
        }
    }
    NXLOG_I(kTag, "low latency: %s (mode %d, backend %s)", enabled_ ? "on" : "off", static_cast<int>(s.cfg.mode), backend_name());
}

LowLatency::~LowLatency() {
    if (g_instance.load() == this) g_instance.store(nullptr);
    if (impl_) {
        if (impl_->sleep_sem) {
            impl_->dev->wait_idle();
            vkDestroySemaphore(impl_->dev->device(), impl_->sleep_sem, nullptr);
        }
        delete impl_;
    }
}

const char* LowLatency::backend_name() const {
    switch (backend_) {
        case Backend::Reflex: return "NVIDIA Reflex (VK_NV_low_latency2)";
        case Backend::AntiLag: return "AMD Anti-Lag 2 (VK_AMD_anti_lag)";
        default: return "none";
    }
}

void LowLatency::prepare_swapchain(VkSwapchainCreateInfoKHR* ci) {
    if (!enabled_ || !impl_->reflex || !ci) return;
    impl_->sci.pNext = const_cast<void*>(ci->pNext);
    ci->pNext = &impl_->sci;
}

void LowLatency::swapchain_created(VkSwapchainKHR sc) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.mu);
    s.swapchain = sc;
    s.mode_set = false;
    if (enabled_) s.set_mode_locked();
}

void LowLatency::swapchain_destroyed(VkSwapchainKHR sc) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.swapchain == sc) {
        s.swapchain = VK_NULL_HANDLE;
        s.mode_set = false;
    }
}

void LowLatency::set_cap_fps(double fps) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.mu);
    s.cap_fps = fps > 0 ? fps : 0;
    if (enabled_) s.set_mode_locked();
}

void LowLatency::sim_begin() {
    if (!impl_->active) return;
    Impl& s = *impl_;
    const uint64_t id = s.sim_id.fetch_add(1) + 1;
    const uint64_t t0 = FrameLimiter::now_ns();
    VkSwapchainKHR sc;
    bool ready;
    {
        std::unique_lock<std::mutex> lk(s.mu);
        // frames_ahead: the simulation of a frame starts only when at most that many earlier frames are still on their way to the screen. The game
        // queues several frames between its main thread and the GPU; this is what keeps that queue short (and so the input sample close to the picture).
        if (s.paced && s.cfg.frames_ahead >= 0) {
            const size_t limit = static_cast<size_t>(s.cfg.frames_ahead);
            s.cv.wait_for(lk, std::chrono::milliseconds(100), [&] { return s.pending.size() <= limit; });
        }
        sc = s.swapchain;
        ready = s.mode_set;
    }
    if (s.reflex && sc && ready) {
        VkLatencySleepInfoNV si = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV};
        si.signalSemaphore = s.sleep_sem;
        si.value = ++s.sleep_value;
        if (vkLatencySleepNV(s.dev->device(), sc, &si) == VK_SUCCESS) {
            VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &s.sleep_sem;
            wi.pValues = &si.value;
            vkWaitSemaphores(s.dev->device(), &wi, 100ull * 1000 * 1000);   // never longer than 100 ms, whatever the driver does
        }
    } else if (s.antilag) {
        s.antilag_update(VK_ANTI_LAG_STAGE_INPUT_AMD, id);
    }
    const double slept = ms_since(t0);
    s.sleep_ms_avg = s.sleep_ms_avg * 0.95 + slept * 0.05;
    s.marker(VK_LATENCY_MARKER_SIMULATION_START_NV, id);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.pending.push_back(id);               // in flight until the present that shows it
        while (s.pending.size() > 16) s.pending.pop_front();
    }
    s.frames++;
    s.poll_reports();
}

void LowLatency::input_sample() {
    if (!impl_->active) return;
    Impl& s = *impl_;
    const uint64_t id = s.sim_id.load();
    s.input_ns_for[id % 64].store(FrameLimiter::now_ns(), std::memory_order_relaxed);
    s.marker(VK_LATENCY_MARKER_INPUT_SAMPLE_NV, id);
}

void LowLatency::sim_end() {
    // SIMULATION_END is set by submit_begin(): the host render thread starting on the frame's commands is the first moment the simulation is known to be done.
}

void LowLatency::submit_begin() {
    if (!impl_->active) return;
    Impl& s = *impl_;
    if (s.submit_marked) return;
    s.submit_marked = true;
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        id = s.pending.empty() ? s.last_present_id + 1 : s.pending.front();
    }
    s.marker(VK_LATENCY_MARKER_SIMULATION_END_NV, id);
    s.marker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, id);
}

uint64_t LowLatency::present_begin() {
    if (!impl_->active) return 0;
    Impl& s = *impl_;
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.pending.empty()) {
            id = s.pending.front();
            s.pending.pop_front();
        } else {
            id = s.last_present_id + 1;          // a frame with no simulation behind it (menus, loading): keep the ids increasing
        }
        if (id <= s.last_present_id) id = s.last_present_id + 1;
    }
    s.cv.notify_all();
    s.present_id = id;
    s.last_present_id = id;
    s.marker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, id);
    s.marker(VK_LATENCY_MARKER_PRESENT_START_NV, id);
    if (s.antilag) s.antilag_update(VK_ANTI_LAG_STAGE_PRESENT_AMD, id);
    s.submit_marked = false;
    return id;
}

void LowLatency::present_end() {
    if (!impl_->active) return;
    Impl& s = *impl_;
    const uint64_t id = s.present_id;
    s.marker(VK_LATENCY_MARKER_PRESENT_END_NV, id);
    const uint64_t in_ns = s.input_ns_for[id % 64].load(std::memory_order_relaxed);
    if (in_ns) {
        const double d = ms_since(in_ns);
        if (d >= 0 && d < 2000) {
            s.in2present_ema = s.have_ema ? s.in2present_ema * 0.95 + d * 0.05 : d;
            s.have_ema = true;
        }
    }
    if (s.cfg.log_timings) {
        const uint64_t n = FrameLimiter::now_ns();
        if (n - s.last_log_ns > 1000000000ull) {
            s.last_log_ns = n;
            NXLOG_I(kTag, "%s: input->present %.2f ms, sleep %.2f ms, input->gpu end %.2f ms, sim->present %.2f ms (%llu driver reports)", backend_name(), s.in2present_ema,
                    s.sleep_ms_avg, s.in2gpu_ema, s.sim2present_ema, static_cast<unsigned long long>(s.reports.load()));
        }
    }
}

LowLatency::Stats LowLatency::stats() const {
    const Impl& s = *impl_;
    Stats st;
    st.backend = backend_;
    st.enabled = enabled_;
    st.frames = s.frames.load();
    st.sleep_ms_avg = s.sleep_ms_avg;
    st.input_to_present_ms = s.in2present_ema;
    st.input_to_gpu_ms = s.in2gpu_ema;
    st.sim_to_present_ms = s.sim2present_ema;
    st.reports = s.reports.load();
    return st;
}

}  // namespace nxrt::extras
