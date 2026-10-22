// SPDX-License-Identifier: GPL-3.0-or-later
// audout:u and audin:u (stream S3c): the output and capture devices an application opens for PCM streams.
// Adapted from ScarletNX src/nx/hle/services/audio.c (GPL-3.0-or-later; that file's audout part is itself ported from Eden,
// GPL-2.0-or-later, as its header says): the command table, the buffer protocol (appended tags, released tags, the buffer event,
// the played-sample count), the refusal of a second Start, and the release of queued buffers on Stop and Flush.
// Adapted from ryubing src/Ryujinx.Horizon/Sdk/Audio/Detail/AudioOut.cs and AudioOutManager.cs (MIT): the channel-count rule of
// OpenAudioOut and the Flush result.
// Layouts and result codes: https://switchbrew.org/wiki/Audio_services and https://switchbrew.org/wiki/Error_codes (module 153).
// Playback runs on one pacing thread per started stream: each buffer is played for frames / rate seconds of host time, its samples
// go to nxrt::audio::sink() when it starts, and its tag is released to the guest when it ends. Capture (audin) is silent: each
// appended buffer is zeroed when it starts and released at real time.
#include "services_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::services {
namespace {

using Clock = std::chrono::steady_clock;

constexpr char kOutName[] = "DeviceOut";
constexpr char kInName[] = "BuiltInHeadset";
constexpr uint32_t kNameSize = 0x100;        // an AudioDeviceName (also AudioOutInfo / AudioInInfo)
constexpr uint32_t kBufferSize = 0x28;       // AudioOutBuffer / AudioInBuffer: next, buffer, capacity, size, offset (Switchbrew)
constexpr uint32_t kDefaultRate = 48000;
constexpr uint32_t kMinRate = 8000, kMaxRate = 192000;
constexpr uint32_t kMaxBuffers = 64;        // queued plus released-but-not-taken buffers per stream (ScarletNX AUDOUT_BUFFERS)
constexpr uint32_t kFormatPcmInt16 = 2;     // nn::audio::SampleFormat PcmInt16
constexpr uint32_t kStateStarted = 0, kStateStopped = 1;
constexpr uint32_t kResNotFound = res(153, 1);
constexpr uint32_t kResOperationFailed = res(153, 2);
constexpr uint32_t kResBufferCountFull = res(153, 8);

// nn::audio::AudioOutBuffer (AudioInBuffer has the same layout). Written out from Switchbrew's table.
struct Desc {
    uint64_t next = 0;      // 0x00 unused
    uint64_t buffer = 0;    // 0x08 guest pointer to the samples
    uint64_t capacity = 0;  // 0x10
    uint64_t size = 0;      // 0x18 bytes of data
    uint64_t offset = 0;    // 0x20 offset of the data in the samples buffer
};

// ---- guest memory --------------------------------------------------------------------------------------------------------
// Guest pointers are host pointers: each range is checked with nxrt::mem::is_mapped before it is read or written.
bool guest_readable(uint64_t addr, uint64_t n) { return addr != 0 && (n == 0 || nxrt::mem::is_mapped(addr, n)); }

bool read_desc(const ipc::Buffer& b, Desc& d) {
    d = Desc{};
    const uint64_t n = b.size < kBufferSize ? b.size : kBufferSize;   // a shorter descriptor leaves the missing fields zero
    if (!guest_readable(b.addr, n)) return false;
    buf_read(b, &d, n);
    return true;
}

std::string read_name(const ipc::Buffer& b) {
    const uint64_t n = b.size < kNameSize ? b.size : kNameSize;
    if (n == 0 || !guest_readable(b.addr, n)) return {};
    std::string s(static_cast<size_t>(n), '\0');
    buf_read(b, s.data(), n);
    return std::string(s.c_str());
}

// Writes a device name zero padded, as many bytes as the buffer holds (at most 0x100).
bool write_name(const ipc::Buffer& b, const char* name) {
    std::array<uint8_t, kNameSize> out{};
    std::memcpy(out.data(), name, std::min<size_t>(std::strlen(name), kNameSize - 1));
    const uint64_t n = b.size < kNameSize ? b.size : kNameSize;
    if (n == 0 || !guest_readable(b.addr, n)) return false;
    buf_write(b, out.data(), n);
    return true;
}

// ---- formats the devices accept -------------------------------------------------------------------------------------------
// The rate an out or in plays at: the default 48 kHz when none (or an unsupported one) is asked for.
uint32_t pick_rate(uint32_t asked) { return asked < kMinRate || asked > kMaxRate ? kDefaultRate : asked; }
// The sink plays 2 or 6 channels (audio_sink.h); any other count falls back to stereo, as Ryujinx does.
uint32_t pick_out_channels(uint32_t asked) { return asked == 6 ? 6 : 2; }
uint32_t pick_in_channels(uint32_t asked) { return asked == 1 || asked == 4 || asked == 6 ? asked : 2; }

// ---- host sink ------------------------------------------------------------------------------------------------------------
// One sink for every stream: it is opened on the first start with the stream's rate and channel count, and opened again when
// a stream asks for another format or the active sink was replaced (nxrt::audio::set_sink).
struct HostSink {
    std::mutex mu;                                   // guards the fields below and every call into the sink
    std::shared_ptr<nxrt::audio::AudioSink> sink;    // the sink opened last
    uint32_t rate = 0, channels = 0;
    bool open_ok = false;
};
HostSink& host() {
    static HostSink h;
    return h;
}

// Caller holds host().mu. True when the sink is open for this format.
bool open_host_locked(uint32_t rate, uint32_t channels) {
    HostSink& h = host();
    auto cur = nxrt::audio::sink();
    if (cur == h.sink && rate == h.rate && channels == h.channels) return h.open_ok;
    h.sink = cur;
    h.rate = rate;
    h.channels = channels;
    h.open_ok = cur->open(rate, channels);
    if (h.open_ok) NXLOG_I("audout", "host sink %s open at %u Hz, %u channels", cur->name(), rate, channels);
    else NXLOG_W("audout", "host sink %s would not open at %u Hz, %u channels: playing without sound", cur->name(), rate, channels);
    return h.open_ok;
}

void play_host(uint32_t rate, uint32_t channels, const int16_t* pcm, uint32_t frames) {
    std::lock_guard<std::mutex> lk(host().mu);
    if (!open_host_locked(rate, channels)) return;
    host().sink->write(pcm, frames);
}

// ---- streams --------------------------------------------------------------------------------------------------------------
enum class Dir { Out, In };

// One buffer appended by the guest. `samples` is 0 when the samples cannot be read (played as silence).
struct Entry {
    uint64_t tag = 0;
    uint64_t samples = 0;
    uint64_t bytes = 0;
    uint32_t frames = 0;
    bool begun = false;
    Clock::time_point begin{};
};

// An open audio out or in: its queue, its released tags, its pacing thread and its buffer event. Shared by the IAudio* object
// (the service) and the registry; the thread only uses it while the object is alive, because every path that drops the object
// stops the thread first (on_close, the destructor).
class Stream {
public:
    Stream(Dir dir, uint32_t rate, uint32_t channels) : dir_(dir), rate_(rate), channels_(channels) {}
    ~Stream() { shutdown(); }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    Dir dir() const { return dir_; }

    // Guest commands. Each takes the stream's locks itself.
    uint32_t start();
    uint32_t stop();
    bool flush();
    uint32_t append(uint64_t tag, const Desc& d);
    uint32_t take_released(uint64_t* out, uint32_t max);
    bool contains(uint64_t tag);
    uint32_t queued();
    uint64_t played();
    uint32_t state();
    float volume();
    void set_volume(float v);
    uint32_t new_event_handle() { return ev_.new_handle(); }

    // Stops the pacing thread and releases what is queued. Used by on_close, reset and the destructor. Idempotent.
    void shutdown();

private:
    void pace();
    void play(const Entry& e);
    void halt();                       // stops and joins the pacing thread; caller holds life_
    void release_all_locked();         // every queued buffer is released now (unplayed)
    void sync_event_locked();          // the buffer event is signalled while a released tag is waiting

    const Dir dir_;
    const uint32_t rate_;
    const uint32_t channels_;

    std::mutex life_;                  // serialises start / stop / shutdown (the thread never takes it)
    std::mutex mu_;                    // guards everything below
    std::condition_variable cv_;
    std::deque<Entry> queue_;          // appended and not yet released; the front is playing when started
    std::deque<uint64_t> released_;    // released tags the guest has not taken yet
    bool started_ = false;
    uint64_t epoch_ = 0;               // bumped when the queue is emptied, so the pacing thread re-reads its head
    uint64_t played_ = 0;             // frames released as played
    float volume_ = 1.0f;
    std::thread th_;
    ServiceEvent ev_;                  // readable while released_ is not empty
};

struct Registry {
    std::mutex mu;
    std::vector<std::weak_ptr<Stream>> streams;
};
Registry& registry() {
    static Registry r;
    return r;
}

std::shared_ptr<Stream> new_stream(Dir dir, uint32_t rate, uint32_t channels) {
    auto s = std::make_shared<Stream>(dir, rate, channels);
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.mu);
    std::erase_if(r.streams, [](const std::weak_ptr<Stream>& w) { return w.expired(); });
    r.streams.push_back(s);
    return s;
}

uint32_t Stream::start() {
    std::lock_guard<std::mutex> life(life_);
    std::lock_guard<std::mutex> lk(mu_);
    if (started_) return kResOperationFailed;
    if (dir_ == Dir::Out) {
        std::lock_guard<std::mutex> hl(host().mu);
        open_host_locked(rate_, channels_);
    }
    started_ = true;
    th_ = std::thread([this] { pace(); });
    return 0;
}

uint32_t Stream::stop() {
    std::lock_guard<std::mutex> life(life_);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!started_) return 0;   // a stopped stream keeps what was appended until it starts
    }
    halt();
    std::lock_guard<std::mutex> lk(mu_);
    release_all_locked();
    return 0;
}

void Stream::shutdown() {
    std::lock_guard<std::mutex> life(life_);
    halt();
    std::lock_guard<std::mutex> lk(mu_);
    release_all_locked();
}

void Stream::halt() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        started_ = false;
        ++epoch_;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

bool Stream::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return false;
    release_all_locked();
    return true;
}

uint32_t Stream::append(uint64_t tag, const Desc& d) {
    Entry e;
    e.tag = tag;
    e.bytes = d.size;
    e.frames = static_cast<uint32_t>(std::min<uint64_t>(d.size / (channels_ * 2u), 0xFFFFFFFFu));
    const bool in_range = d.offset <= d.capacity && d.size <= d.capacity - d.offset;
    if (in_range && d.size && guest_readable(d.buffer + d.offset, d.size)) {
        e.samples = d.buffer + d.offset;
    } else if (d.size) {
        NXLOG_W("audout", "sample buffer %#llx (%llu bytes at %llu of %llu) is not readable: played as silence",
                static_cast<unsigned long long>(d.buffer), static_cast<unsigned long long>(d.size),
                static_cast<unsigned long long>(d.offset), static_cast<unsigned long long>(d.capacity));
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.size() + released_.size() >= kMaxBuffers) return kResBufferCountFull;
    queue_.push_back(e);
    cv_.notify_all();
    return 0;
}

uint32_t Stream::take_released(uint64_t* out, uint32_t max) {
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t n = 0;
    while (n < max && !released_.empty()) {
        out[n++] = released_.front();
        released_.pop_front();
    }
    sync_event_locked();
    return n;
}

bool Stream::contains(uint64_t tag) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const Entry& e : queue_)
        if (e.tag == tag) return true;
    for (uint64_t t : released_)
        if (t == tag) return true;
    return false;
}

uint32_t Stream::queued() {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<uint32_t>(queue_.size());
}

uint64_t Stream::played() {
    std::lock_guard<std::mutex> lk(mu_);
    return played_;
}

uint32_t Stream::state() {
    std::lock_guard<std::mutex> lk(mu_);
    return started_ ? kStateStarted : kStateStopped;
}

float Stream::volume() {
    std::lock_guard<std::mutex> lk(mu_);
    return volume_;
}

void Stream::set_volume(float v) {
    std::lock_guard<std::mutex> lk(mu_);
    volume_ = v;
}

void Stream::release_all_locked() {
    for (const Entry& e : queue_) released_.push_back(e.tag);
    queue_.clear();
    ++epoch_;
    cv_.notify_all();
    sync_event_locked();
}

void Stream::sync_event_locked() {
    if (released_.empty()) ev_.clear();
    else ev_.signal();
}

// The samples of one buffer: written to the sink (out) or zeroed (in). Runs without mu_ held.
void Stream::play(const Entry& e) {
    if (e.frames == 0) return;
    const uint64_t n = static_cast<uint64_t>(e.frames) * channels_ * 2u;
    const bool readable = e.samples != 0 && nxrt::mem::is_mapped(e.samples, n);
    if (dir_ == Dir::In) {
        if (readable) std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(e.samples)), 0, static_cast<size_t>(n));
        return;
    }
    std::vector<int16_t> pcm(static_cast<size_t>(e.frames) * channels_, 0);   // silence when the samples cannot be read
    if (readable) std::memcpy(pcm.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(e.samples)), static_cast<size_t>(n));
    play_host(rate_, channels_, pcm.data(), e.frames);
}

// The pacing thread. The head of the queue plays for frames / rate seconds from the moment it starts; then it is released.
// Halt, flush and stop bump epoch_, which sends the loop back to re-read its head.
void Stream::pace() {
    std::unique_lock<std::mutex> lk(mu_);
    while (started_) {
        if (queue_.empty()) {
            cv_.wait(lk, [this] { return !started_ || !queue_.empty(); });
            continue;
        }
        Entry& head = queue_.front();
        const uint64_t epoch = epoch_;
        if (!head.begun) {
            head.begun = true;
            head.begin = Clock::now();
            const Entry copy = head;
            lk.unlock();
            play(copy);
            lk.lock();
            if (!started_ || epoch_ != epoch) continue;
        }
        const auto span = std::chrono::nanoseconds(static_cast<uint64_t>(head.frames) * 1000000000ull / rate_);
        const Clock::time_point deadline = head.begin + span;
        cv_.wait_until(lk, deadline, [this, epoch] { return !started_ || epoch_ != epoch; });
        if (!started_ || epoch_ != epoch) continue;
        played_ += head.frames;
        released_.push_back(head.tag);
        queue_.pop_front();
        sync_event_locked();
    }
}

// ---- services -------------------------------------------------------------------------------------------------------------
// The IAudioOut / IAudioIn object. Its stream is shut down (pacing thread joined) when the session that holds it goes away.
class StreamService : public ipc::Service {
public:
    StreamService(const char* name, std::shared_ptr<Stream> s) : ipc::Service(name), st_(std::move(s)) {}
    void on_close() override { st_->shutdown(); }

private:
    std::shared_ptr<Stream> st_;
};

// AppendAudioOutBuffer / AppendAudioInBuffer (and the Auto forms): tag, then the AudioOutBuffer descriptor in buffer A.
uint32_t cmd_append(Stream& s, ipc::Request& req) {
    Desc d;
    if (!read_desc(req.in_buf(0), d)) return kResOperationFailed;
    return s.append(req.arg<uint64_t>(0), d);
}

// GetReleasedAudioOutBuffers / GetReleasedAudioInBuffers (and the Auto forms): fills the tag array in buffer B and returns the count.
// The array is zeroed past the tags, as the SDK reads every slot.
uint32_t cmd_released(Stream& s, ipc::Request& req, ipc::Response& rsp) {
    const ipc::Buffer b = req.out_buf(0);
    const uint64_t bytes = b.size < kMaxBuffers * 8u ? b.size / 8 * 8 : kMaxBuffers * 8u;
    if (bytes == 0 || !guest_readable(b.addr, bytes)) {
        rsp.push<uint32_t>(0);
        return 0;
    }
    std::array<uint64_t, kMaxBuffers> tags{};
    const uint32_t n = s.take_released(tags.data(), static_cast<uint32_t>(bytes / 8));
    buf_write(b, tags.data(), bytes);
    rsp.push<uint32_t>(n);
    return 0;
}

std::shared_ptr<ipc::Service> make_out_stream(std::shared_ptr<Stream> st) {
    Stream* s = st.get();
    auto svc = std::make_shared<StreamService>("IAudioOut", std::move(st));
    svc->add_cmd(0, "GetAudioOutState", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(s->state());
        return 0;
    });
    svc->add_cmd(1, "Start", [s](ipc::Request&, ipc::Response&) -> uint32_t {
        const uint32_t rc = s->start();
        if (rc == 0) NXLOG_I("audout", "Start");
        return rc;
    });
    svc->add_cmd(2, "Stop", [s](ipc::Request&, ipc::Response&) -> uint32_t { return s->stop(); });
    svc->add_cmd(3, "AppendAudioOutBuffer", [s](ipc::Request& req, ipc::Response&) -> uint32_t { return cmd_append(*s, req); });
    svc->add_cmd(4, "RegisterBufferEvent", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.out_copy.push_back(s->new_event_handle());
        return 0;
    });
    svc->add_cmd(5, "GetReleasedAudioOutBuffers", [s](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        return cmd_released(*s, req, rsp);
    });
    svc->add_cmd(6, "ContainsAudioOutBuffer", [s](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(s->contains(req.arg<uint64_t>(0)) ? 1 : 0);
        return 0;
    });
    svc->add_cmd(7, "AppendAudioOutBufferAuto", [s](ipc::Request& req, ipc::Response&) -> uint32_t { return cmd_append(*s, req); });
    svc->add_cmd(8, "GetReleasedAudioOutBuffersAuto", [s](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        return cmd_released(*s, req, rsp);
    });
    svc->add_cmd(9, "GetAudioOutBufferCount", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(s->queued());
        return 0;
    });
    svc->add_cmd(10, "GetAudioOutPlayedSampleCount", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint64_t>(s->played());
        return 0;
    });
    svc->add_cmd(11, "FlushAudioOutBuffers", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(s->flush() ? 1 : 0);
        return 0;
    });
    svc->add_cmd(12, "SetAudioOutVolume", [s](ipc::Request& req, ipc::Response&) -> uint32_t {
        s->set_volume(req.arg<float>(0));
        return 0;
    });
    svc->add_cmd(13, "GetAudioOutVolume", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<float>(s->volume());
        return 0;
    });
    return svc;
}

// IAudioIn: the buffer protocol of IAudioOut (no UAC buffers and no device gain: not implemented).
std::shared_ptr<ipc::Service> make_in_stream(std::shared_ptr<Stream> st) {
    Stream* s = st.get();
    auto svc = std::make_shared<StreamService>("IAudioIn", std::move(st));
    svc->add_cmd(0, "GetAudioInState", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(s->state());
        return 0;
    });
    svc->add_cmd(1, "Start", [s](ipc::Request&, ipc::Response&) -> uint32_t {
        const uint32_t rc = s->start();
        if (rc == 0) NXLOG_I("audin", "Start");
        return rc;
    });
    svc->add_cmd(2, "Stop", [s](ipc::Request&, ipc::Response&) -> uint32_t { return s->stop(); });
    svc->add_cmd(3, "AppendAudioInBuffer", [s](ipc::Request& req, ipc::Response&) -> uint32_t { return cmd_append(*s, req); });
    svc->add_cmd(4, "RegisterBufferEvent", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.out_copy.push_back(s->new_event_handle());
        return 0;
    });
    svc->add_cmd(5, "GetReleasedAudioInBuffers", [s](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        return cmd_released(*s, req, rsp);
    });
    svc->add_cmd(6, "ContainsAudioInBuffer", [s](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(s->contains(req.arg<uint64_t>(0)) ? 1 : 0);
        return 0;
    });
    svc->add_cmd(8, "AppendAudioInBufferAuto", [s](ipc::Request& req, ipc::Response&) -> uint32_t { return cmd_append(*s, req); });
    svc->add_cmd(9, "GetReleasedAudioInBuffersAuto", [s](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        return cmd_released(*s, req, rsp);
    });
    svc->add_cmd(11, "GetAudioInBufferCount", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(s->queued());
        return 0;
    });
    svc->add_cmd(14, "FlushAudioInBuffers", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(s->flush() ? 1 : 0);
        return 0;
    });
    return svc;
}

// ListAudioOuts / ListAudioIns (and the Auto forms): one name when the output buffer holds one, the count returned.
uint32_t list_names(ipc::Request& req, ipc::Response& rsp, const char* name) {
    uint32_t count = 0;
    if (write_name(req.out_buf(0), name)) count = 1;
    rsp.push<uint32_t>(count);
    return 0;
}

// OpenAudioOut: AudioOutParameter {u32 SampleRate, u16 ChannelCount, u16 reserved} at 0, the applet resource id at 8 (not used).
// Returns AudioOutParameterInternal {rate, channels, sample format, state} and the IAudioOut object.
uint32_t open_out(ipc::Request& req, ipc::Response& rsp) {
    const std::string want = read_name(req.in_buf(0));
    if (!want.empty() && want != kOutName) return kResNotFound;
    const uint32_t rate = pick_rate(req.arg<uint32_t>(0));
    const uint32_t ch = pick_out_channels(req.arg<uint16_t>(4));
    NXLOG_I("audout", "OpenAudioOut: %s, %u Hz, %u channels", kOutName, rate, ch);
    write_name(req.out_buf(0), kOutName);
    rsp.push<uint32_t>(rate);
    rsp.push<uint32_t>(ch);
    rsp.push<uint32_t>(kFormatPcmInt16);
    rsp.push<uint32_t>(kStateStopped);
    rsp.push_object(make_out_stream(new_stream(Dir::Out, rate, ch)));
    return 0;
}

// OpenAudioIn: AudioInParameter, same layout as AudioOutParameter. Returns AudioInParameterInternal and the IAudioIn object.
uint32_t open_in(ipc::Request& req, ipc::Response& rsp) {
    const std::string want = read_name(req.in_buf(0));
    if (!want.empty() && want != kInName) return kResNotFound;
    const uint32_t rate = pick_rate(req.arg<uint32_t>(0));
    const uint32_t ch = pick_in_channels(req.arg<uint16_t>(4));
    NXLOG_I("audin", "OpenAudioIn: %s, %u Hz, %u channels", kInName, rate, ch);
    write_name(req.out_buf(0), kInName);
    rsp.push<uint32_t>(rate);
    rsp.push<uint32_t>(ch);
    rsp.push<uint32_t>(kFormatPcmInt16);
    rsp.push<uint32_t>(kStateStopped);
    rsp.push_object(make_in_stream(new_stream(Dir::In, rate, ch)));
    return 0;
}

std::shared_ptr<ipc::Service> make_audout() {
    return make_iface("audout:u", [](ipc::Service& s) {
        s.add_cmd(0, "ListAudioOuts", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return list_names(req, rsp, kOutName); });
        s.add_cmd(1, "OpenAudioOut", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return open_out(req, rsp); });
        s.add_cmd(2, "ListAudioOutsAuto", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return list_names(req, rsp, kOutName); });
        s.add_cmd(3, "OpenAudioOutAuto", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return open_out(req, rsp); });
    });
}

std::shared_ptr<ipc::Service> make_audin() {
    return make_iface("audin:u", [](ipc::Service& s) {
        s.add_cmd(0, "ListAudioIns", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return list_names(req, rsp, kInName); });
        s.add_cmd(1, "OpenAudioIn", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return open_in(req, rsp); });
        s.add_cmd(2, "ListAudioInsAuto", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return list_names(req, rsp, kInName); });
        s.add_cmd(3, "OpenAudioInAuto", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return open_in(req, rsp); });
        s.add_cmd(4, "ListAudioInsAutoFiltered", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return list_names(req, rsp, kInName); });
    });
}

}  // namespace

void register_audio_out() {
    add("audout:u", [] { return make_audout(); });
    add("audin:u", [] { return make_audin(); });
}

// Stops every pacing thread and forgets the streams. Sessions still holding an object keep a working but unregistered stream.
void audio_out_reset() {
    std::vector<std::shared_ptr<Stream>> live;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lk(r.mu);
        for (const auto& w : r.streams)
            if (auto s = w.lock()) live.push_back(std::move(s));
        r.streams.clear();
    }
    for (auto& s : live) s->shutdown();
    {
        std::lock_guard<std::mutex> lk(host().mu);
        host().sink.reset();
        host().rate = 0;
        host().channels = 0;
        host().open_ok = false;
    }
}

}  // namespace nxrt::services
