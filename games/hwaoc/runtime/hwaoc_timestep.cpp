// SPDX-License-Identifier: GPL-3.0-or-later
// Age of Calamity's frame-step hooks (hwaoc_timestep.h, docs/FRAMESTEP.md). All sites are in recomp/addrs.tsv.
//
// Threads: the KIDS frame task (kTaskWait, kTaskHead, kObjStep, kClockStep) runs once per update on one thread; it publishes this update's
// ticks in atomics that the tick sites (any job thread) and the scene update's cutscene counters read.
#include "hwaoc_timestep.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "hwaoc_addrs.h"
#include "hwaoc_settings.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/kernel.h"
#include "nxrt/timestep.h"

namespace nxrt::games::hwaoc {

namespace {

constexpr const char* kTag = "hwaoc.time";
constexpr float kRetailStep = 1.0f / 30.0f;      // interval 2 at 60 Hz: the step every 30 fps mechanism assumes
constexpr uint32_t kMaxTicks = 4;                // whole 60 Hz ticks handed to one update at most (the base clamps the step to 1/30 s)

struct TickSite {
    uint64_t offset;
    uint8_t reg;
};
const TickSite kTicks[] = {
#include "hwaoc_timestep_sites.h"
};

struct S {
    std::vector<uint32_t> hooks;
    double acc60 = 0.0, acc30 = 0.0;             // fractional 60 Hz and 30 Hz ticks carried to the next update
    std::atomic<uint32_t> ticks_bits{0};         // this update's ticks (float bits, 60 Hz units)
    std::atomic<uint32_t> fps_bits{0};           // 1 / step (float bits)
    std::atomic<int32_t> cut_frames{1};          // whole 30 fps cutscene frames of this update
    std::atomic<bool> logged_thread{false};
};

S& s() {
    static S* inst = new S();                     // never destroyed: hooks may still run during process teardown
    return *inst;
}

uint32_t f2u(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float u2f(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
void set_s(::Ctx* c, int reg, float v) {
    c->v[reg].u64[0] = 0;
    c->v[reg].u64[1] = 0;
    c->v[reg].f32[0] = v;
}
// The hooks act while the base hands out a real step and the player has not chosen the retail step.
bool on() { return timestep::active() && !retail_step_chosen(); }

template <class T>
T* guest(uint64_t addr) {
    return reinterpret_cast<T*>(addr);
}

// ---- the KIDS frame task -----------------------------------------------------------------------------------------------------------------
// udiv x26, x24, x20: the period the task waits for since its last update (tick frequency / fps). The base already held this update to the
// frame cap at the task's entry, so the game's own wait (30 fps) must not hold it again: x20 = a rate no update reaches. [sp] keeps the
// game's fps for the scheduler call that receives it.
int hook_task_wait(::Ctx* c, uint64_t, void*) {
    if (!on()) return 0;
    c->x[20] = 1000000;
    return 0;
}

// After the wait, every path: this update's ticks and the step the game's 30 fps data reads.
int hook_task_head(::Ctx*, uint64_t, void*) {
    if (!on()) return 0;
    S& st = s();
    if (!st.logged_thread.exchange(true)) {
        const kernel::KThread* th = kernel::current_thread();
        NXLOG_I(kTag, "frame task on guest thread %llu '%s' (prio %d)", static_cast<unsigned long long>(th ? th->id : 0), th ? th->name.c_str() : "?", th ? th->prio : -1);
    }
    const double dt = timestep::current_dt();
    const double t60 = dt * 60.0, t30 = dt * 30.0;
    st.acc60 += t60;
    const uint32_t whole = static_cast<uint32_t>(std::min<double>(std::floor(st.acc60), kMaxTicks));
    st.acc60 = std::clamp(st.acc60 - whole, 0.0, 1.0);
    st.acc30 += t30;
    const int32_t cut = static_cast<int32_t>(std::min(std::floor(st.acc30), 2.0));
    st.acc30 = std::clamp(st.acc30 - cut, 0.0, 1.0);
    const float fps = static_cast<float>(1.0 / dt);
    st.ticks_bits.store(f2u(static_cast<float>(t60)), std::memory_order_release);
    st.fps_bits.store(f2u(fps), std::memory_order_release);
    st.cut_frames.store(cut, std::memory_order_release);

    // The frame-rate block {fps, interval, fps, float fps}
    const uint64_t block = mem::kMainBase + addr::kFrameRateBlock;
    const uint32_t ifps = static_cast<uint32_t>(std::lround(fps));
    *guest<uint32_t>(block + 0) = ifps;
    *guest<uint32_t>(block + 4) = whole;
    *guest<uint32_t>(block + 8) = ifps;
    *guest<float>(block + 12) = fps;
    // The KTGL device's frame rate (59.94 / interval on the Switch), which the event scripts step by
    const uint64_t dev = *guest<uint64_t>(mem::kMainBase + addr::kDevicePointer);
    if (dev && mem::guest_readable(dev + 0x32d0, 4)) *guest<float>(dev + 0x32d0) = static_cast<float>(59.94 / t60);
    return 0;
}

// The frame-rate object's two steps (x23 = the object: +0x34 and +0x44 are timescale / its fps, +0x48 its fps) become timescale x the real
// time: w8 (stored at [x19,#0x18] already, and stored again further on) and s0.
int hook_obj_step(::Ctx* c, uint64_t, void*) {
    if (!on()) return 0;
    const uint64_t obj = c->x[23];
    const int32_t obj_fps = *guest<int32_t>(obj + 0x48);
    if (obj_fps <= 0) return 0;
    const float k = static_cast<float>(obj_fps) * timestep::current_dt();
    const float step = u2f(static_cast<uint32_t>(c->x[8])) * k;
    c->x[8] = f2u(step);
    *guest<float>(c->x[19] + 0x18) = step;
    set_s(c, 0, c->v[0].f32[0] * k);
    return 0;
}

// No frame-rate object (loading): the step is 1 / the task's fps or the measured time; both become the real time.
int hook_clock_step(::Ctx* c, uint64_t, void*) {
    if (!on()) return 0;
    const float dt = timestep::current_dt();
    c->x[8] = f2u(dt);
    *guest<float>(c->x[19] + 0x18) = dt;
    set_s(c, 0, dt);
    return 0;
}

// ---- readers of the block ----------------------------------------------------------------------------------------------------------------
int hook_tick(::Ctx* c, uint64_t, void* user) {
    if (!on()) return 0;
    set_s(c, static_cast<const TickSite*>(user)->reg, u2f(s().ticks_bits.load(std::memory_order_acquire)));
    return 0;
}

int hook_fps_recip(::Ctx* c, uint64_t, void*) {
    if (!on()) return 0;
    set_s(c, 1, u2f(s().fps_bits.load(std::memory_order_acquire)));
    return 0;
}

// An event cutscene's frame (+1 per update) and frames left (-1): w10 already holds the stepped value; step by the 30 fps frames of real time.
int hook_cut_frame(::Ctx* c, uint64_t, void*) {
    if (!on()) return 0;
    const int32_t n = s().cut_frames.load(std::memory_order_acquire);
    c->x[10] = static_cast<uint32_t>(static_cast<int32_t>(c->x[10]) - 1 + n);
    return 0;
}
int hook_cut_left(::Ctx* c, uint64_t, void*) {
    if (!on()) return 0;
    const int32_t n = s().cut_frames.load(std::memory_order_acquire);
    c->x[10] = static_cast<uint32_t>(static_cast<int32_t>(c->x[10]) + 1 - n);
    return 0;
}

void install(uint64_t base) {
    S& st = s();
    st.acc60 = st.acc30 = 0.0;
    st.ticks_bits.store(f2u(2.0f));
    st.fps_bits.store(f2u(30.0f));
    st.cut_frames.store(1);
    st.hooks.push_back(hook_add_mid(base + addr::kTaskWait, &hook_task_wait));
    st.hooks.push_back(hook_add_mid(base + addr::kTaskHead, &hook_task_head));
    st.hooks.push_back(hook_add_mid(base + addr::kObjStep, &hook_obj_step));
    st.hooks.push_back(hook_add_mid(base + addr::kClockStep, &hook_clock_step));
    st.hooks.push_back(hook_add_mid(base + addr::kFpsRecip, &hook_fps_recip));
    st.hooks.push_back(hook_add_mid(base + addr::kCutFrame, &hook_cut_frame));
    st.hooks.push_back(hook_add_mid(base + addr::kCutLeft, &hook_cut_left));
    for (const TickSite& t : kTicks) st.hooks.push_back(hook_add_mid(base + t.offset, &hook_tick, const_cast<TickSite*>(&t)));
    NXLOG_I(kTag, "frame step hooks: %zu (%zu interval readers)", st.hooks.size(), sizeof kTicks / sizeof kTicks[0]);
}

void remove() {
    S& st = s();
    for (uint32_t id : st.hooks) hook_remove(id);
    st.hooks.clear();
}

}  // namespace

const timestep::Sites& timestep_sites() {
    static const timestep::Sites sites = [] {
        timestep::Sites t;
        t.retail_step = kRetailStep;
        t.install = &install;
        t.remove = &remove;
        return t;
    }();
    return sites;
}

}  // namespace nxrt::games::hwaoc
