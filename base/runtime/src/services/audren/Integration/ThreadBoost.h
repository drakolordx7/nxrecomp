// SPDX-License-Identifier: GPL-3.0-or-later
// Scheduling for the audio renderer's two threads (the DSP thread and the thread that paces it). Not in the Ryujinx port. The renderer has
// to deliver a 5 ms frame every 5 ms while the game's render and loading threads keep every core busy; a frame that comes late is a click
// on the sound card. The threads ask the host for timely scheduling (Windows: the Multimedia Class Scheduler Service task "Pro Audio";
// Linux: the highest band the nice value allows) and the system timer runs at 1 ms while the renderer is up, so the pacing sleeps are not
// rounded up to 15 ms. The host calls live in nxrt/os.
#pragma once
#include "nxrt/os/thread.h"
#include "nxrt/os/time.h"

namespace nxrt::audren {

// Call at the top of an audio thread. The host drops the registration when the thread ends.
inline void BoostAudioThread() { os::thread_boost_audio(); }

// Raises the calling thread's priority for the lifetime of the object and restores it afterwards. Used by the thread that updates the
// renderer while it holds the renderer's lock (a stand-in for priority inheritance).
class ScopedAudioLockBoost {
  public:
    ScopedAudioLockBoost() : _thread(os::thread_current()), _old(os::thread_get_band(_thread)) {
        if (static_cast<int>(_old) < static_cast<int>(os::PriorityBand::Highest)) {
            _changed = os::thread_set_band(_thread, os::PriorityBand::Highest);
        }
    }
    ~ScopedAudioLockBoost() {
        if (_changed) {
            os::thread_set_band(_thread, _old);
        }
    }
    ScopedAudioLockBoost(const ScopedAudioLockBoost&) = delete;
    ScopedAudioLockBoost& operator=(const ScopedAudioLockBoost&) = delete;

  private:
    os::ThreadHandle _thread;
    os::PriorityBand _old;
    bool _changed = false;
};

// Holds the 1 ms timer resolution for as long as any renderer runs (reference counted by the os layer).
class TimerResolutionScope {
  public:
    static void Acquire() { os::timer_resolution_acquire(); }
    static void Release() { os::timer_resolution_release(); }
};

}
