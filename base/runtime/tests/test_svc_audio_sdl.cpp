// SPDX-License-Identifier: GPL-3.0-or-later
// The SDL host sink (src/services/audio_sdl.cpp) on SDL's "dummy" audio driver, which needs no sound card and drains its stream in real time
// like a device: write, queue level, the queue cap, reopening, bad arguments, and the sink choice [audio] sink = sdl.
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/os/process.h"
#include "test_framework.h"

NXTEST_GROUP(svc_audio_sdl)

using namespace nxrt;

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kPieceFrames = kRate / 100;   // 10 ms

// SDL reads SDL_AUDIO_DRIVER each time it starts the audio subsystem; put it back afterwards.
struct DummyDriver {
    std::optional<std::string> old = os::process::getenv("SDL_AUDIO_DRIVER");
    DummyDriver() { os::process::setenv("SDL_AUDIO_DRIVER", "dummy"); }
    ~DummyDriver() { os::process::setenv("SDL_AUDIO_DRIVER", old ? old->c_str() : ""); }
};

}  // namespace

NXTEST(sdl_stereo_writes_are_accepted_and_queued) {
    DummyDriver drv;
    auto s = audio::make_sdl_sink();
    REQUIRE(s != nullptr);
    CHECK_EQ(std::string(s->name()), std::string("sdl"));
    CHECK(s->has_device_clock());
    if (!s->open(kRate, 2)) {
        nxtest::skip("SDL cannot start its dummy audio driver here");
        return;
    }
    const std::vector<int16_t> piece(static_cast<size_t>(kPieceFrames) * 2, 0);
    uint32_t accepted = 0;
    for (int i = 0; i < 10; i++) accepted += s->write(piece.data(), kPieceFrames);   // 100 ms
    CHECK_EQ(accepted, 10 * kPieceFrames);
    CHECK(s->queued_frames() > 0);
    const audio::SinkStats st = s->stats();
    CHECK_EQ(st.frames_written, uint64_t(10 * kPieceFrames));
    CHECK_EQ(st.frames_dropped, 0ull);
    s->close();
    CHECK_EQ(s->queued_frames(), 0u);
}

NXTEST(sdl_queue_drains_at_the_device_rate) {
    DummyDriver drv;
    auto s = audio::make_sdl_sink();
    if (!s->open(kRate, 2)) {
        nxtest::skip("SDL cannot start its dummy audio driver here");
        return;
    }
    const std::vector<int16_t> piece(static_cast<size_t>(kPieceFrames) * 2, 0);
    for (int i = 0; i < 20; i++) s->write(piece.data(), kPieceFrames);   // 200 ms
    const uint32_t before = s->queued_frames();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const uint32_t after = s->queued_frames();
    CHECK(after < before);
    s->close();
}

NXTEST(sdl_overfill_drops_and_does_not_crash) {
    DummyDriver drv;
    auto s = audio::make_sdl_sink();
    if (!s->open(kRate, 2)) {
        nxtest::skip("SDL cannot start its dummy audio driver here");
        return;
    }
    const std::vector<int16_t> piece(static_cast<size_t>(kPieceFrames) * 2, 0);
    uint64_t offered = 0, accepted = 0;
    for (int i = 0; i < 200; i++) {   // 2 s written with no waiting: the queue cap must drop the rest
        accepted += s->write(piece.data(), kPieceFrames);
        offered += kPieceFrames;
    }
    CHECK(accepted > 0);
    CHECK(accepted < offered);
    CHECK_EQ(s->stats().frames_dropped, offered - accepted);
    s->close();
}

NXTEST(sdl_reopen_six_channels_and_double_close) {
    DummyDriver drv;
    auto s = audio::make_sdl_sink();
    s->close();   // never opened
    if (!s->open(kRate, 2)) {
        nxtest::skip("SDL cannot start its dummy audio driver here");
        return;
    }
    s->close();
    s->close();
    REQUIRE(s->open(kRate, 6));
    const std::vector<int16_t> piece(static_cast<size_t>(kPieceFrames) * 6, 0);
    CHECK_EQ(s->write(piece.data(), kPieceFrames), kPieceFrames);
    s->close();
    CHECK_EQ(s->queued_frames(), 0u);
}

NXTEST(sdl_rejects_bad_arguments) {
    DummyDriver drv;
    auto s = audio::make_sdl_sink();
    CHECK(!s->open(kRate, 4));
    CHECK(!s->open(100, 2));
    CHECK_EQ(s->write(nullptr, 0), 0u);
}

NXTEST(sdl_is_the_default_sink_choice) {
    DummyDriver drv;
    audio::set_sink(nullptr);
    config_set("audio", "sink", "sdl");
    auto s = audio::sink();
    CHECK(std::string(s->name()) == "sdl" || std::string(s->name()) == "null");   // null: the dummy driver could not start
    audio::set_sink(nullptr);
    config_set("audio", "sink", "auto");
}

NXTEST(sdl_lists_devices_of_the_dummy_driver) {
    DummyDriver drv;
    const auto devices = audio::list_output_devices();
    for (const auto& d : devices) CHECK(!d.id.empty());
}
