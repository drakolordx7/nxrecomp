// SPDX-License-Identifier: GPL-3.0-or-later
// Alternating measurement windows: one helper for the paired A/B switches ([nvn] gpl_ab_pause and gpuperf_ab in presents, [latency]
// ab_toggle_s in nanoseconds). The ticks from `origin` are cut into windows of `period` ticks, numbered from 0; each switch decides what the
// even and the odd windows do, so a run shows both states under the same load. A period of 0 means the switch is off.
#pragma once
#include <atomic>
#include <cstdint>

namespace nxrt::gfx {

class AbWindows {
public:
    AbWindows() = default;
    AbWindows(uint64_t period, uint64_t origin) : period_(period), origin_(origin) {}
    bool active() const { return period_ > 0; }
    // The window `tick` falls in (0 before the origin or while off).
    uint64_t window(uint64_t tick) const { return active() && tick > origin_ ? (tick - origin_) / period_ : 0; }
    bool odd(uint64_t tick) const { return (window(tick) & 1) != 0; }
    // True on the first call in a new window (any thread); *w receives the window. For the one log line or mark per switch.
    bool entered(uint64_t tick, uint64_t* w = nullptr) {
        const uint64_t cur = window(tick);
        if (w) *w = cur;
        return last_.exchange(static_cast<int64_t>(cur)) != static_cast<int64_t>(cur);
    }
    uint64_t period() const { return period_; }
    void reset(uint64_t period, uint64_t origin) {
        period_ = period;
        origin_ = origin;
        last_.store(-1);
    }

private:
    uint64_t period_ = 0, origin_ = 0;
    std::atomic<int64_t> last_{-1};
};

}  // namespace nxrt::gfx
