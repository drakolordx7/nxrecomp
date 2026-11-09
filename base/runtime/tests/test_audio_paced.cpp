// SPDX-License-Identifier: GPL-3.0-or-later
// The paced sink (src/services/audio_paced.cpp): a simulated output device that drains its queue at the sample rate on the host clock.
// The audio renderer paces itself by the queue level of such a device (AudioProcessor::Wait), so these cases pin down what the sink reports.
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "nxrt/audio_sink.h"
#include "test_framework.h"

NXTEST_GROUP(svc_audio_paced)

using namespace nxrt;

namespace {
constexpr uint32_t kRate = 48000;

// Polls the sink's queue until `done(queued)` holds, without a fixed wait: the host clock drains the queue, however slowly a loaded
// machine schedules this thread. Every sample must be no higher than the one before (a queue only drains between writes).
template <class F>
bool poll_until(audio::AudioSink& sink, F done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    uint32_t prev = sink.queued_frames();
    while (!done(prev)) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const uint32_t q = sink.queued_frames();
        if (q > prev) return false;
        prev = q;
    }
    return true;
}
}  // namespace

NXTEST(paced_has_a_device_clock_and_the_null_sink_does_not) {
    auto p = audio::make_paced_sink();
    CHECK(p->has_device_clock());
    CHECK_EQ(std::string(p->name()), std::string("paced"));
    audio::NullSink n;
    CHECK(!n.has_device_clock());
}

NXTEST(paced_queue_drains_to_empty_and_counts_the_accepted_frames) {
    auto p = audio::make_paced_sink();
    REQUIRE(p->open(kRate, 2));
    std::vector<int16_t> pcm(static_cast<size_t>(kRate / 10) * 2, 0);
    CHECK_EQ(p->write(pcm.data(), kRate / 10), kRate / 10);
    CHECK(p->queued_frames() <= kRate / 10);        // never more than was written
    CHECK(poll_until(*p, [](uint32_t q) { return q < kRate / 10 - 480; }));   // the host clock drains it
    CHECK(poll_until(*p, [](uint32_t q) { return q == 0; }));
    audio::SinkStats s = p->stats();
    CHECK_EQ(s.underruns, 1ull);                    // it ran dry once, after everything was played
    CHECK_EQ(s.frames_accepted, static_cast<uint64_t>(kRate / 10));
    p->close();
}

NXTEST(paced_counts_one_underrun_per_dry_stretch) {
    auto p = audio::make_paced_sink();
    REQUIRE(p->open(kRate, 2));
    std::vector<int16_t> pcm(static_cast<size_t>(480) * 2, 0);
    p->write(pcm.data(), 480);
    CHECK(poll_until(*p, [](uint32_t q) { return q == 0; }));          // the queue ran dry: one underrun
    CHECK_EQ(p->queued_frames(), 0u);
    CHECK_EQ(p->queued_frames(), 0u);                                   // asking again does not count again
    CHECK_EQ(p->stats().underruns, 1ull);
    p->write(pcm.data(), 480);
    CHECK(poll_until(*p, [](uint32_t q) { return q == 0; }));
    CHECK_EQ(p->stats().underruns, 2ull);
    p->close();
}

NXTEST(paced_drops_what_does_not_fit) {
    auto p = audio::make_paced_sink();
    REQUIRE(p->open(kRate, 2));
    std::vector<int16_t> pcm(static_cast<size_t>(kRate) * 2, 0);        // one second at once: the queue holds 120 ms
    const uint32_t took = p->write(pcm.data(), kRate);
    CHECK(took > kRate / 10 && took < kRate / 5);
    const audio::SinkStats s = p->stats();
    CHECK_EQ(s.frames_written, static_cast<uint64_t>(kRate));
    CHECK_EQ(s.frames_accepted + s.frames_dropped, static_cast<uint64_t>(kRate));
    p->close();
}

NXTEST(paced_skew_changes_the_drain_rate) {
    // A device skewed 99 % slow drains 100 ms of audio in 10 s; one skewed 90 % fast drains it in about 53 ms. Once the fast one has
    // run dry the slow one must still hold nearly everything, however long the machine took to get there.
    auto fast = audio::make_paced_sink(900000.0);
    auto slow = audio::make_paced_sink(-990000.0);
    REQUIRE(fast->open(kRate, 2) && slow->open(kRate, 2));
    std::vector<int16_t> pcm(static_cast<size_t>(kRate / 10) * 2, 0);
    fast->write(pcm.data(), kRate / 10);
    slow->write(pcm.data(), kRate / 10);
    CHECK(poll_until(*fast, [](uint32_t q) { return q == 0; }));
    CHECK(slow->queued_frames() > kRate / 20);
    fast->close();
    slow->close();
}
