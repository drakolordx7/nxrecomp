// SPDX-License-Identifier: GPL-3.0-or-later
// Low-latency modes (stream S14): NVIDIA Reflex through VK_NV_low_latency2 and AMD Anti-Lag 2 through VK_AMD_anti_lag.
//
// One frame of the game crosses three threads: the guest main thread (Update: pacing, input sampling, simulation), the host render thread
// that replays the game's command lists and presents, and the driver. The markers follow that:
//
//   main thread    sim_begin()        sleeps (Reflex vkLatencySleepNV / Anti-Lag INPUT stage) so the frame starts as late as the GPU allows, then
//                                     SIMULATION_START; input_sample() sets INPUT_SAMPLE just before the mouse and pad are read; sim_end()
//   render thread  submit_begin()     RENDERSUBMIT_START at the first replayed submission of the frame
//                  present_begin()    RENDERSUBMIT_END, PRESENT_START, returns the present id to pass in VkPresentIdKHR
//                  present_end()      PRESENT_END
//
// Frame ids pair the threads: a frame leaves sim_end() into a queue and the next present takes the oldest id, so a frame keeps its id from
// simulation to present however many frames the render thread lags. All calls are no-ops when the mode is off or the GPU has neither
// extension, and every entry point is safe to call from any thread.
#pragma once
#include <cstdint>
#include <string>

#include "nxrt/extras.h"
#include "nxrt/gfx.h"

namespace nxrt::extras {

class LowLatency {
public:
    enum class Backend : uint8_t { None = 0, Reflex, AntiLag };

    // Reads the settings; the backend is the one the GPU supports (Reflex first) when the mode is not Off.
    explicit LowLatency(gfx::Device& dev);
    ~LowLatency();
    LowLatency(const LowLatency&) = delete;
    LowLatency& operator=(const LowLatency&) = delete;

    Backend backend() const { return backend_; }
    bool enabled() const { return enabled_; }
    const char* backend_name() const;

    // Swapchain life cycle (render thread). prepare_swapchain chains VkSwapchainLatencyCreateInfoNV into ci (storage lives in *this until the
    // call returns) when Reflex is enabled; swapchain_created / swapchain_destroyed tell the module the handle (the sleep mode is per swapchain).
    void prepare_swapchain(VkSwapchainCreateInfoKHR* ci);
    void swapchain_created(VkSwapchainKHR sc);
    void swapchain_destroyed(VkSwapchainKHR sc);
    // The frame cap Reflex / Anti-Lag pace to (0 = none). Called when the cap changes.
    void set_cap_fps(double fps);

    // Main thread.
    void sim_begin();
    void input_sample();
    void sim_end();
    // Render thread.
    void submit_begin();
    uint64_t present_begin();     // the id for VkPresentIdKHR (0 when disabled)
    void present_end();

    // Timings. Reflex: the driver's own frame reports; always: the host proxy input sample -> present returned.
    struct Stats {
        Backend backend = Backend::None;
        bool enabled = false;
        uint64_t frames = 0;
        double sleep_ms_avg = 0;          // time spent blocked in sim_begin
        double input_to_present_ms = 0;   // proxy: INPUT_SAMPLE to the end of vkQueuePresentKHR (EMA)
        double input_to_gpu_ms = 0;       // Reflex reports: inputSample to gpuRenderEnd (EMA; 0 when no reports)
        double sim_to_present_ms = 0;     // Reflex reports: simStart to presentEnd
        uint64_t reports = 0;
    };
    Stats stats() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    Backend backend_ = Backend::None;
    bool enabled_ = false;
};

// The instance of the running renderer (set by the renderer when it creates one; null otherwise). Hooks and the replay code go through it.
LowLatency* lowlat();
void lowlat_set(LowLatency* l);

}  // namespace nxrt::extras
