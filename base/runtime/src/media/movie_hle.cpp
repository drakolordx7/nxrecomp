// SPDX-License-Identifier: GPL-3.0-or-later
// movie:: (Nintendo's multimedia library, a port of Android's stagefright): the protocol layer. See include/nxrt/movie.h. Demuxing and decoding
// are done by a MovieSource (include/nxrt/movie_source.h): FFmpeg on both platforms (src/media/movie_ffmpeg.cpp), Windows Media Foundation as a
// fallback (src/os/win32/movie_mf.cpp, [media] backend = mf).
//
// The protocol the game speaks was worked out for smo-recomp (runtime/services/nnmovie_mf.c, GPL-3.0, same library in another game):
// the Extractor/Decoder objects, the buffer model of the decoder, the two-pool index scheme, the "token" trick that lets the source's
// demux+decode stand in for a compressed sample. This file is a port of that design to nxrt, with these changes: the
// game here calls the plain exported wrappers (not virtual functions), runs its decoders without events, so everything is polled;
// the source runs in a worker thread per movie (decode-ahead queues per track) so the game's threads never wait for a
// decode; edit lists of the MP4 are honoured (the first frame is at 0, the AAC priming is cut), as ffmpeg does.
//
// Layouts read off the game's main (v1.0.0, IDA) and off subsdk0:
//   movie::Extractor  16 bytes embedded in the game's extractor object   {host vtable, impl*}
//   movie::Decoder    16 bytes embedded in the game's decoder object     {host vtable, impl*}
//   movie::MediaData  8 bytes, one pointer to a key/value store          {impl*}
//   movie::Buffer     0x40 bytes: +0 offset i32, +4 size i32, +c flags u32, +0x10 owns u8, +0x18 capacity u64, +0x20 i64, +0x28 base
//   ClientStreamReader (the game's reader object, vtable at +0 of the object): +0x10 Open(path, void** handle) -> bool,
//     +0x18 ReadAt(handle, offset, buf, n) -> bytes, +0x20 Close(handle), +0x28 GetSize(handle, int64*)
// Results: 0 ok, 0x2000 end of data (GetTrackIndexForAvailableData / GetSampleTime / Read), 0x1011 no input buffer, 0x100e no output
// buffer, 0x100b bad argument, 0x1000 failure.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <initializer_list>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/movie.h"
#include "nxrt/movie_source.h"
#include "nxrt/os/fs.h"
#include "nxrt/services.h"

namespace nxrt::media {
namespace {

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define A5 (c->x[5])
#define RET(v) (c->x[0] = static_cast<uint64_t>(v))

constexpr int ST_OK = 0;
constexpr int ST_FAILED = 0x1000;
constexpr int ST_INVALID = 0x100b;
constexpr int ST_UNSUPPORTED = 0x1006;
constexpr int ST_END = 0x2000;
constexpr int ST_NO_INPUT = 0x1011;
constexpr int ST_NO_OUTPUT = 0x100e;
constexpr uint32_t FLAG_EOS = 4;
constexpr size_t kSlotBytes = 64;
constexpr uint64_t kTokenMagic = 0x4d4f564945544f4bull;   // "MOVIETOK"
// Byte 0..7 of an Extractor or Decoder the host owns: a vtable of host thunks (object_vtable below), so a game that calls the members
// through the vtable (older SDKs) reaches the same handlers as one that calls the exported wrappers.
uint64_t ext_vtable();
uint64_t dec_vtable();
constexpr int kAudioChunk = 0x8000;                   // what GetOutputBufferSize says for audio (the game allocates at least this)
constexpr size_t kVideoQueueMax = 6, kAudioQueueMax = 64, kOtherQueueMax = 8;
constexpr int kWaitMs = 8000;                         // longest the extractor makes a game thread wait for the worker

bool g_log = false, g_use_mf = false;
bool g_lookahead = true, g_preroll = true, g_async_close = true;
std::atomic<uint64_t> s_extractors_created{0}, s_extractors_destroyed{0}, s_decoders_created{0}, s_decoders_destroyed{0}, s_frames{0},
    s_audio_blocks{0}, s_audio_bytes{0}, s_samples{0}, s_read_errors{0};
GaugeCounter g_media_workers("media.worker_threads");

// ---- guest memory --------------------------------------------------------------------------------------------------------------
inline uint64_t rd64(uint64_t a) { uint64_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 8); return v; }
inline uint32_t rd32(uint64_t a) { uint32_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 4); return v; }
inline void wr64(uint64_t a, uint64_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 8); }
inline void wr32(uint64_t a, uint32_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 4); }
inline void wr8(uint64_t a, uint8_t v) { *reinterpret_cast<uint8_t*>(a) = v; }
inline uint8_t rd8(uint64_t a) { return *reinterpret_cast<const uint8_t*>(a); }

// Calls a guest function with up to five arguments from inside a handler; returns x0. The handler's own argument registers are the
// caller's business (they are saved here only so a handler may keep using them after the call).
uint64_t gcall(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t save[9];
    std::memcpy(save, c->x, sizeof save);
    const uint64_t lr = c->x[30], f = c->fn;
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    std::memcpy(c->x, save, sizeof save);
    c->x[30] = lr;
    c->fn = f;
    return r;
}

// ---- movie::MediaData ----------------------------------------------------------------------------------------------------------
struct MDEntry {
    enum Type { I32, I64, STR, PTR, RECT } t = I32;
    int64_t i = 0;
    int32_t r[4] = {0, 0, 0, 0};     // RECT: left, top, right, bottom (inclusive, as stagefright's "crop")
    std::string s;
    uint64_t p = 0;
};
struct MD {
    std::map<std::string, MDEntry> e;
    void set_i32(const char* k, int32_t v) { MDEntry& x = e[k]; x.t = MDEntry::I32; x.i = v; }
    void set_i64(const char* k, int64_t v) { MDEntry& x = e[k]; x.t = MDEntry::I64; x.i = v; }
    void set_str(const char* k, const char* v) { MDEntry& x = e[k]; x.t = MDEntry::STR; x.s = v ? v : ""; }
    void set_ptr(const char* k, uint64_t v) { MDEntry& x = e[k]; x.t = MDEntry::PTR; x.p = v; }
    void set_rect(const char* k, int32_t l, int32_t t, int32_t r, int32_t b) {
        MDEntry& x = e[k];
        x.t = MDEntry::RECT;
        x.r[0] = l; x.r[1] = t; x.r[2] = r; x.r[3] = b;
    }
    const MDEntry* find(const char* k) const { auto it = e.find(k); return it == e.end() ? nullptr : &it->second; }
    bool get_i32(const char* k, int32_t* out) const {
        const MDEntry* x = find(k);
        if (!x || (x->t != MDEntry::I32 && x->t != MDEntry::I64)) return false;
        *out = static_cast<int32_t>(x->i);
        return true;
    }
    bool get_i64(const char* k, int64_t* out) const {
        const MDEntry* x = find(k);
        if (!x || (x->t != MDEntry::I32 && x->t != MDEntry::I64)) return false;
        *out = x->i;
        return true;
    }
};
MD* md_of(uint64_t guest) { return guest ? reinterpret_cast<MD*>(rd64(guest)) : nullptr; }

void mdata_ctor(::Ctx* c) { wr64(A0, reinterpret_cast<uint64_t>(new MD())); }
void mdata_copy(::Ctx* c) {
    MD* src = md_of(A1);
    MD* m = new MD();
    if (src) m->e = src->e;
    wr64(A0, reinterpret_cast<uint64_t>(m));
}
void mdata_dtor(::Ctx* c) { delete md_of(A0); wr64(A0, 0); }
void mdata_clear(::Ctx* c) { if (MD* m = md_of(A0)) m->e.clear(); }
void mdata_set_i32(::Ctx* c) {               // SetInt32(const char* key, int32 value)
    if (MD* m = md_of(A0); m && A1) m->set_i32(reinterpret_cast<const char*>(A1), static_cast<int32_t>(A2));
}
void mdata_find_rect(::Ctx* c) {             // FindRect(const char* key, int* left, int* top, int* right, int* bottom)
    MD* m = md_of(A0);
    const MDEntry* x = m && A1 ? m->find(reinterpret_cast<const char*>(A1)) : nullptr;
    const bool ok = x && x->t == MDEntry::RECT;
    if (ok) {
        const uint64_t out[4] = {A2, A3, A4, A5};
        for (int k = 0; k < 4; k++)
            if (out[k]) wr32(out[k], static_cast<uint32_t>(x->r[k]));
    }
    RET(ok);
}
void mdata_find_i32(::Ctx* c) {
    int32_t v = 0;
    MD* m = md_of(A0);
    const bool ok = m && A1 && m->get_i32(reinterpret_cast<const char*>(A1), &v);
    if (ok && A2) wr32(A2, static_cast<uint32_t>(v));
    RET(ok);
}
void mdata_find_i64(::Ctx* c) {
    int64_t v = 0;
    MD* m = md_of(A0);
    const bool ok = m && A1 && m->get_i64(reinterpret_cast<const char*>(A1), &v);
    if (ok && A2) wr64(A2, static_cast<uint64_t>(v));
    RET(ok);
}
void mdata_find_str(::Ctx* c) {
    MD* m = md_of(A0);
    const MDEntry* x = m && A1 ? m->find(reinterpret_cast<const char*>(A1)) : nullptr;
    const bool ok = x && x->t == MDEntry::STR;
    if (ok && A2) wr64(A2, reinterpret_cast<uint64_t>(x->s.c_str()));
    RET(ok);
}

// ---- movie::Buffer -------------------------------------------------------------------------------------------------------------
constexpr uint64_t B_OFFSET = 0x00, B_SIZE = 0x04, B_FLAGS = 0x0c, B_OWNS = 0x10, B_CAP = 0x18, B_I64 = 0x20, B_BASE = 0x28;
void buf_ctor(::Ctx* c) { std::memset(reinterpret_cast<void*>(A0), 0, 0x40); }
void buf_ctor_mem(::Ctx* c) {            // Buffer(void* base, size_t capacity): memory the game owns
    std::memset(reinterpret_cast<void*>(A0), 0, 0x40);
    wr64(A0 + B_BASE, A1);
    wr64(A0 + B_CAP, A2);
}
void buf_dtor(::Ctx* c) { wr64(A0 + B_BASE, 0); wr64(A0 + B_CAP, 0); }   // the game's buffers never own their memory
void buf_base(::Ctx* c) { RET(rd64(A0 + B_BASE)); }
void buf_size(::Ctx* c) { RET(rd32(A0 + B_SIZE)); }
void buf_offset(::Ctx* c) { RET(rd32(A0 + B_OFFSET)); }

// ---- the extractor -------------------------------------------------------------------------------------------------------------
using SamplePtr = std::shared_ptr<SourceSample>;
struct Item {
    SamplePtr s;
    int64_t us = 0;
};
struct Track {
    int kind = 2;
    std::string mime;
    int w = 0, h = 0, rate = 0, ch = 0, bitrate = 0, colorspace = 2;
    double fps = 0;
    int64_t offset_us = 0;
    bool selected = false, eos = false, failed = false;
    std::deque<Item> q;
    int64_t last_us = INT64_MIN;
    size_t limit() const { return kind == 0 ? kVideoQueueMax : kind == 1 ? kAudioQueueMax : kOtherQueueMax; }
};

struct Ext;
// Every live extractor and decoder, for the gauges near the end of this file. Lock order there: the registry first, then the object's m.
std::mutex g_exts_mu;
std::set<Ext*> g_exts;

struct Ext {
    Ext() { std::lock_guard<std::mutex> lk(g_exts_mu); g_exts.insert(this); }
    ~Ext() { std::lock_guard<std::mutex> lk(g_exts_mu); g_exts.erase(this); }
    Ext(const Ext&) = delete;
    Ext& operator=(const Ext&) = delete;
    int container = 1;
    std::string name;
    std::unique_ptr<MovieSource> src;
    std::unique_ptr<unsigned char[]> file;   // the movie data the source reads from (it must outlive the source: src is released first)
    std::vector<Track> tr;
    int64_t duration_us = 0;
    std::mutex m;
    std::mutex reader_mu;            // the source: one call at a time (read, seek, track selection)
    std::condition_variable cv_data, cv_work;
    std::thread worker;
    bool stop = false, started = false;
    bool seek_pending = false;
    bool reading = false;            // set by the first request for data: the game has selected its tracks by then (a stream selected after reading began would start late)
    int64_t seek_us = 0;
    // Loop look-ahead (S21): when every selected track has reached its end the worker positions the reader at the start and decodes the first
    // samples into `nx`, so a SeekTo(0) that follows (a looping movie) swaps them in at once instead of waiting for the decoder to start again.
    struct Ahead {
        std::deque<Item> q;
        bool eos = false, failed = false;
        int64_t last_us = INT64_MIN;
    };
    std::vector<Ahead> nx;
    int ahead = 0;
    bool ahead_tried = false;
    uint32_t swaps = 0;              // look-aheads swapped in so far: a sample read for the look-ahead while it was swapped in belongs to the queue that replaced it
    // the current sample as NuMediaExtractor defines it: the earliest head among the selected tracks
    bool cur_valid = false;
    int cur_track = -1;
    int64_t cur_us = 0;
    uint64_t samples_read = 0;
};

Ext* ext_of(uint64_t guest) { return guest && rd64(guest) == ext_vtable() ? reinterpret_cast<Ext*>(rd64(guest + 8)) : nullptr; }

bool ext_open(Ext* e, unsigned char* data, size_t size) {
    e->src = g_use_mf ? make_mf_source() : make_ffmpeg_source();
    if (!e->src) { NXLOG_W("movie", "no %s backend available", g_use_mf ? "Media Foundation" : "FFmpeg"); return false; }
    if (!e->src->open(data, size, e->container)) { e->src.reset(); return false; }
    e->duration_us = e->src->duration_us();
    std::vector<Track> tracks;
    for (const SourceTrack& st : e->src->tracks()) {
        Track t;
        t.kind = st.kind; t.mime = st.mime; t.w = st.w; t.h = st.h; t.rate = st.rate; t.ch = st.ch; t.bitrate = st.bitrate;
        t.colorspace = st.colorspace; t.fps = st.fps; t.offset_us = st.offset_us;
        tracks.push_back(std::move(t));
    }
    {   // under e->m: the gauges read the track queues from the sampler thread
        std::lock_guard<std::mutex> lk(e->m);
        e->tr = std::move(tracks);
    }
    std::string desc;
    for (size_t i = 0; i < e->tr.size(); i++) {
        const Track& t = e->tr[i];
        char b[160];
        if (t.kind == 0) std::snprintf(b, sizeof b, " [%zu] %s %dx%d %.2f fps (edit offset %lld us)", i, t.mime.c_str(), t.w, t.h, t.fps, static_cast<long long>(t.offset_us));
        else if (t.kind == 1) std::snprintf(b, sizeof b, " [%zu] %s %d Hz %d ch (edit offset %lld us)", i, t.mime.c_str(), t.rate, t.ch, static_cast<long long>(t.offset_us));
        else std::snprintf(b, sizeof b, " [%zu] %s", i, t.mime.c_str());
        desc += b;
    }
    NXLOG_I("movie", "opened %zu bytes, %.3f s, %zu tracks:%s", size, e->duration_us / 1e6, e->tr.size(), desc.c_str());
    return true;
}

int ext_select_locked(Ext* e, int i, bool on) {
    if (!e->src || i < 0 || i >= static_cast<int>(e->tr.size())) return ST_INVALID;
    Track& t = e->tr[static_cast<size_t>(i)];
    if (e->reading) NXLOG_W("movie", "track %d selected after reading began: the stream may start late", i);
    SelectResult sr;
    SourceTrack upd;
    {
        std::lock_guard<std::mutex> rl(e->reader_mu);
        sr = e->src->select(i, on, &upd);
    }
    if (sr == SelectResult::Unsupported) {
        NXLOG_W("movie", "track %d (%s): no decoder path; left unselected", i, t.mime.c_str());
        return ST_UNSUPPORTED;
    }
    {
        std::lock_guard<std::mutex> lk(e->m);
        if (on && sr == SelectResult::Ok && t.kind != 2) { t.w = upd.w; t.h = upd.h; t.rate = upd.rate; t.ch = upd.ch; }
        t.selected = on && sr == SelectResult::Ok;
        e->nx.clear();
        e->ahead = 0;
        e->ahead_tried = false;
        t.eos = false;
        t.failed = false;
        t.q.clear();
        t.last_us = INT64_MIN;
        e->cur_valid = false;
    }
    e->cv_work.notify_all();
    NXLOG_I("movie", "%s track %d (%s)", on ? "selected" : "unselected", i, t.mime.c_str());
    return sr == SelectResult::Ok ? ST_OK : ST_FAILED;
}

void worker_main(Ext* e) {
    struct Alive {
        Alive() { g_media_workers.add(1); }
        ~Alive() { g_media_workers.sub(1); }
    } alive;
    log_set_thread_name("movie-read");
    std::unique_lock<std::mutex> lk(e->m);
    while (!e->stop) {
        if (e->seek_pending) {
            const int64_t us = e->seek_us;
            lk.unlock();
            bool ok;
            {
                std::lock_guard<std::mutex> rl(e->reader_mu);
                ok = e->src->seek(us);
            }
            lk.lock();
            for (Track& t : e->tr) { t.q.clear(); t.eos = false; t.failed = false; t.last_us = INT64_MIN; }
            e->nx.clear();
            e->ahead = 0;
            e->ahead_tried = false;
            e->cur_valid = false;
            e->seek_pending = false;
            NXLOG_I("movie", "SeekTo(%.3f s) %s", us / 1e6, ok ? "ok" : "failed");
            e->cv_data.notify_all();
            continue;
        }
        if (g_lookahead && e->reading && e->ahead == 0 && !e->ahead_tried && !e->seek_pending) {
            bool any = false, all_done = true;
            for (const Track& t : e->tr) {
                if (!t.selected) continue;
                any = true;
                if (!t.eos || t.failed) all_done = false;
            }
            if (any && all_done) {
                e->ahead_tried = true;
                e->ahead = 1;
                lk.unlock();
                bool ok;
                {
                    std::lock_guard<std::mutex> rl(e->reader_mu);
                    ok = e->src->seek(0);
                }
                lk.lock();
                if (e->ahead == 1 && ok && !e->seek_pending && !e->stop) {
                    e->nx.assign(e->tr.size(), Ext::Ahead());
                    e->ahead = 2;
                    NXLOG_D("movie", "end of stream: the start is being decoded ahead for a loop");
                } else {
                    e->ahead = 0;
                }
                e->cv_data.notify_all();
                continue;
            }
        }
        const bool ah_read = e->ahead == 2;
        const uint32_t swaps0 = e->swaps;
        const bool ah_pick = ah_read;
        bool ah = ah_pick;
        Track* pick = nullptr;
        for (Track& t : e->tr) {
            if (!e->reading) break;
            const size_t k = static_cast<size_t>(&t - e->tr.data());
            const bool t_eos = ah ? e->nx[k].eos : t.eos, t_failed = ah ? e->nx[k].failed : t.failed;
            const size_t qn = ah ? e->nx[k].q.size() : t.q.size();
            const int64_t last = ah ? e->nx[k].last_us : t.last_us;
            if (!t.selected || t_eos || t_failed || qn >= t.limit()) continue;
            if (!pick || last < (ah ? e->nx[static_cast<size_t>(pick - e->tr.data())].last_us : pick->last_us)) pick = &t;
        }
        if (!pick) { e->cv_work.wait(lk); continue; }
        const int kind = pick->kind;
        const int64_t offset_us = pick->offset_us;
        const int rate = pick->rate, ch = pick->ch;
        const size_t ti = static_cast<size_t>(pick - e->tr.data());
        lk.unlock();
        SamplePtr s = std::make_shared<SourceSample>();
        ReadStatus rs;
        {
            std::lock_guard<std::mutex> rl(e->reader_mu);
            rs = e->src->read(static_cast<int>(ti), s.get());
        }
        lk.lock();
        if (e->seek_pending || e->stop) continue;
        if ((e->ahead == 2) != ah_pick) {
            // the look-ahead changed while the read ran: swapped in (the sample is the next one of the queue that replaced it) or dropped (it belongs to a position nobody wants)
            if (ah_pick && e->swaps != swaps0) ah = false;
            else continue;
        }
        Track& t = e->tr[ti];
        std::deque<Item>& Q = ah ? e->nx[ti].q : t.q;
        bool& EOS = ah ? e->nx[ti].eos : t.eos;
        bool& FAIL = ah ? e->nx[ti].failed : t.failed;
        int64_t& LAST = ah ? e->nx[ti].last_us : t.last_us;
        if (rs == ReadStatus::Error) {
            s_read_errors++;
            NXLOG_W("movie", "read(track %zu) failed after %llu samples: treated as its end", ti, static_cast<unsigned long long>(e->samples_read));
            FAIL = EOS = true;
            e->cv_data.notify_all();
            continue;
        }
        if (g_log) NXLOG_I("movie", "read(track %zu): status %d ts %lld", ti, static_cast<int>(rs), static_cast<long long>(s->us));
        if (rs == ReadStatus::End) {
            NXLOG_D("movie", "track %zu: end of stream after %zu queued", ti, Q.size());
            EOS = true;
            e->cv_data.notify_all();
            continue;
        }
        if (rs == ReadStatus::Again) continue;
        Item it;
        it.us = s->us + offset_us;
        it.s = s;
        e->samples_read++;
        s_samples++;
        bool keep = true;
        if (it.us < 0) {
            if (kind == 1 && rate > 0 && ch > 0) {                 // audio before the presentation start: priming, cut it
                const uint64_t frame_bytes = static_cast<uint64_t>(ch) * 2;
                const uint64_t drop = (static_cast<uint64_t>(-it.us) * static_cast<uint64_t>(rate) + 999999) / 1000000 * frame_bytes;
                if (drop >= s->data.size()) keep = false;
                else { s->data.erase(s->data.begin(), s->data.begin() + static_cast<std::ptrdiff_t>(drop)); it.us = 0; }
            } else {
                keep = false;
            }
        }
        if (keep) {
            LAST = it.us;
            Q.push_back(std::move(it));
        }
        e->cv_data.notify_all();
    }
}

void ext_start_worker(Ext* e) {
    if (e->started) return;
    e->started = true;
    e->worker = std::thread(worker_main, e);
}

void ext_destroy(Ext* e) {
    {
        std::lock_guard<std::mutex> lk(e->m);
        e->stop = true;
    }
    e->cv_work.notify_all();
    e->cv_data.notify_all();
    if (e->worker.joinable()) e->worker.join();
    e->src.reset();
}

// The game's destructor call returns at once (S21): the worker may be inside a read and tearing a source down stops its decoder
// (60 to 80 ms with Media Foundation), which would otherwise stall the game thread that closes the movie. A short-lived thread does the join and the release.
void ext_destroy_async(Ext* e) {
    {
        std::lock_guard<std::mutex> lk(e->m);
        e->stop = true;
    }
    e->cv_work.notify_all();
    e->cv_data.notify_all();
    try {
        std::thread([e] {
            log_set_thread_name("movie-close");
            ext_destroy(e);
            delete e;
        }).detach();
    } catch (...) {
        ext_destroy(e);
        delete e;
    }
}

bool ext_ready_locked(const Ext* e) {
    if (e->seek_pending) return false;
    for (const Track& t : e->tr)
        if (t.selected && t.q.empty() && !t.eos && !t.failed) return false;
    return true;
}

// The extractor's current sample: the one with the smallest timestamp among the next samples of the selected tracks. Waits (bounded)
// until every selected track that has not ended has a decoded sample queued. Caller holds `lk` on e->m.
int ext_fill(Ext* e, std::unique_lock<std::mutex>& lk) {
    if (e->cur_valid) return ST_OK;
    if (!e->reading) { e->reading = true; e->cv_work.notify_all(); }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    for (;;) {
        bool ready = !e->seek_pending;
        bool any_selected = false;
        for (const Track& t : e->tr) {
            if (!t.selected) continue;
            any_selected = true;
            if (t.q.empty() && !t.eos && !t.failed) ready = false;
        }
        if (!any_selected) return ST_END;
        if (ready) break;
        if (e->cv_data.wait_until(lk, deadline) == std::cv_status::timeout) {
            NXLOG_W("movie", "the worker did not deliver a sample in %d ms", kWaitMs);
            return ST_FAILED;
        }
    }
    int best = -1;
    for (size_t i = 0; i < e->tr.size(); i++) {
        const Track& t = e->tr[i];
        if (!t.selected || t.q.empty()) continue;
        if (best < 0 || t.q.front().us < e->tr[static_cast<size_t>(best)].q.front().us) best = static_cast<int>(i);
    }
    if (best < 0) return ST_END;
    e->cur_valid = true;
    e->cur_track = best;
    e->cur_us = e->tr[static_cast<size_t>(best)].q.front().us;
    return ST_OK;
}

// ---- Extractor handlers --------------------------------------------------------------------------------------------------------
void ext_ctor(::Ctx* c) {                    // Extractor(ContainerType[, CacheSize]): 1 mp4, 2 mkv, 3 webm
    Ext* e = new Ext();
    e->container = static_cast<int>(A1);
    wr64(A0, ext_vtable());
    wr64(A0 + 8, reinterpret_cast<uint64_t>(e));
    s_extractors_created++;
    NXLOG_I("movie", "Extractor created (container type %d)", e->container);
}
void ext_dtor(::Ctx* c) {
    Ext* e = ext_of(A0);
    if (!e) return;
    wr64(A0 + 8, 0);
    wr64(A0, 0);
    NXLOG_I("movie", "Extractor destroyed (%llu samples read)", static_cast<unsigned long long>(e->samples_read));
    if (g_async_close) {
        ext_destroy_async(e);
    } else {
        ext_destroy(e);
        delete e;
    }
    s_extractors_destroyed++;
}

// The movie data is owned by the extractor from here on: the source reads from it until the extractor is destroyed.
int ext_open_data(Ext* e, std::unique_ptr<unsigned char[]> data, size_t size) {
    if (e->container == 1 && (size < 12 || std::memcmp(data.get() + 4, "ftyp", 4) != 0)) NXLOG_W("movie", "the movie data does not start with an MP4 'ftyp' box");
    if (!ext_open(e, data.get(), size)) return ST_FAILED;
    e->file = std::move(data);
    ext_start_worker(e);
    return ST_OK;
}

void ext_set_source(::Ctx* c) {              // SetDataSource(ClientStreamReader*, const char* path)
    Ext* e = ext_of(A0);
    const uint64_t reader = A1;
    const char* path = reinterpret_cast<const char*>(A2);
    if (!e || !reader) { RET(ST_INVALID); return; }
    if (path) e->name = path;
    const uint64_t vt = rd64(reader);
    uint64_t handle = 0;
    const uint64_t ok = gcall(c, rd64(vt + 0x10), reader, A2, reinterpret_cast<uint64_t>(&handle), 0, 0);
    if (!(ok & 1)) { NXLOG_W("movie", "SetDataSource: the game's reader could not open the movie"); RET(ST_FAILED); return; }
    int64_t size = 0;
    gcall(c, rd64(vt + 0x28), reader, handle, reinterpret_cast<uint64_t>(&size), 0, 0);
    if (size <= 0 || size > (int64_t{2} << 30)) {
        NXLOG_W("movie", "SetDataSource: size %lld", static_cast<long long>(size));
        gcall(c, rd64(vt + 0x20), reader, handle, 0, 0, 0);
        RET(ST_FAILED);
        return;
    }
    std::unique_ptr<unsigned char[]> data(new (std::nothrow) unsigned char[static_cast<size_t>(size)]);
    if (!data) { gcall(c, rd64(vt + 0x20), reader, handle, 0, 0, 0); RET(ST_FAILED); return; }
    const auto t0 = std::chrono::steady_clock::now();
    int64_t got = 0;
    while (got < size) {
        const uint64_t chunk = std::min<uint64_t>(static_cast<uint64_t>(size - got), 8u << 20);
        const int64_t n = static_cast<int64_t>(gcall(c, rd64(vt + 0x18), reader, handle, static_cast<uint64_t>(got), reinterpret_cast<uint64_t>(data.get() + got), chunk));
        if (n <= 0) break;
        got += n;
    }
    gcall(c, rd64(vt + 0x20), reader, handle, 0, 0, 0);
    NXLOG_I("movie", "SetDataSource: read %lld of %lld bytes through the game's reader in %.2f s", static_cast<long long>(got), static_cast<long long>(size),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    if (got != size) { RET(ST_FAILED); return; }
    RET(ext_open_data(e, std::move(data), static_cast<size_t>(size)));
}
// SetDataSource(const char* path) of the older SDKs: the library opens the file itself. The path names a file of the game's data
// ("rom:/movie/x.webm" or "movie/x.webm"): the mount name is dropped and the file is read from the RomFS folder.
void ext_set_source_path(::Ctx* c) {
    Ext* e = ext_of(A0);
    const char* path = reinterpret_cast<const char*>(A1);
    if (!e || !path) { RET(ST_INVALID); return; }
    e->name = path;
    std::string rel = path;
    if (const size_t colon = rel.find(':'); colon != std::string::npos) rel.erase(0, colon + 1);
    while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(0, 1);
    const std::string host = services::romfs_dir() + "/" + rel;
    std::FILE* f = os::fs::fopen(host, "rb");
    if (!f) { NXLOG_W("movie", "SetDataSource(%s): no file %s", path, host.c_str()); RET(ST_FAILED); return; }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::unique_ptr<unsigned char[]> data(size > 0 && size <= (2l << 30) ? new (std::nothrow) unsigned char[static_cast<size_t>(size)] : nullptr);
    const bool ok = data && std::fread(data.get(), 1, static_cast<size_t>(size), f) == static_cast<size_t>(size);
    std::fclose(f);
    if (!ok) { NXLOG_W("movie", "SetDataSource(%s): cannot read %s", path, host.c_str()); RET(ST_FAILED); return; }
    NXLOG_I("movie", "SetDataSource(%s): %ld bytes from the RomFS folder", path, size);
    RET(ext_open_data(e, std::move(data), static_cast<size_t>(size)));
}

void ext_track_count(::Ctx* c) {
    Ext* e = ext_of(A0);
    if (!e || !A1) { RET(ST_INVALID); return; }
    wr32(A1, static_cast<uint32_t>(e->src ? e->tr.size() : 0));
    RET(e->src ? ST_OK : ST_FAILED);
}

void ext_track_config(::Ctx* c) {            // GetTrackConfiguration(int track, MediaData* out)
    Ext* e = ext_of(A0);
    const int i = static_cast<int>(A1);
    MD* m = md_of(A2);
    if (!e || !m || !e->src || i < 0 || i >= static_cast<int>(e->tr.size())) { RET(ST_INVALID); return; }
    const Track& t = e->tr[static_cast<size_t>(i)];
    m->e.clear();
    m->set_str("mime", t.mime.c_str());
    m->set_i64("durationUs", e->duration_us);
    m->set_i32("max-input-size", 0x10000);
    if (t.bitrate > 0) m->set_i32("bit-rate", t.bitrate);
    if (t.kind == 0) {
        m->set_i32("width", t.w);
        m->set_i32("height", t.h);
        m->set_i32("frame-rate", static_cast<int>(t.fps + 0.5));
        m->set_i32("nn-colorspace", t.colorspace);
    } else if (t.kind == 1) {
        m->set_i32("sample-rate", t.rate);
        m->set_i32("channel-count", t.ch);
    }
    m->set_ptr("nn-extractor", reinterpret_cast<uint64_t>(e));
    m->set_i32("nn-track", i);
    RET(ST_OK);
}

// Called when a track has just been selected (S21). Once every audio and video track is selected, decoding starts here and the call waits for
// the first decoded samples, on the thread that sets the movie up (the game's loader), so that the first GetSampleTime of the game's main thread finds
// them queued. The wait is bounded. With only some of the tracks selected nothing happens: a track selected after reading began would start late.
void ext_preroll(Ext* e) {
    if (!g_preroll) return;
    std::unique_lock<std::mutex> lk(e->m);
    if (e->reading) return;
    bool any = false;
    for (const Track& t : e->tr) {
        if (t.kind == 2) continue;
        if (!t.selected) return;
        any = true;
    }
    if (!any) return;
    const auto t0 = std::chrono::steady_clock::now();
    e->reading = true;
    e->cv_work.notify_all();
    const auto deadline = t0 + std::chrono::milliseconds(2000);
    while (!ext_ready_locked(e)) {
        if (e->cv_data.wait_until(lk, deadline) == std::cv_status::timeout) {
            NXLOG_W("movie", "preroll: no decoded sample after 2 s");
            return;
        }
    }
    NXLOG_I("movie", "preroll: first samples decoded in %.1f ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void ext_select(::Ctx* c) {
    Ext* e = ext_of(A0);
    if (!e) { RET(ST_INVALID); return; }
    const int r = ext_select_locked(e, static_cast<int>(A1), true);
    if (r == ST_OK) ext_preroll(e);
    RET(r);
}

void ext_seek(::Ctx* c) {                    // SeekTo(int64 timeUs)
    Ext* e = ext_of(A0);
    if (!e || !e->src) { RET(ST_INVALID); return; }
    int64_t us = static_cast<int64_t>(A1);
    std::unique_lock<std::mutex> lk(e->m);
    if (us > 0) {                                  // the target is in presentation time; the reader's clock runs by the video's edit offset ahead
        for (const Track& t : e->tr) if (t.selected && t.kind == 0) { us = std::max<int64_t>(0, us - t.offset_us); break; }
    } else {
        us = 0;
    }
    if (us == 0 && e->ahead != 0) {
        const auto wait_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
        while (e->ahead == 1 && e->cv_data.wait_until(lk, wait_until) != std::cv_status::timeout) {}
        if (e->ahead == 2 && !e->seek_pending && e->nx.size() == e->tr.size()) {
            size_t n = 0;
            for (size_t i = 0; i < e->tr.size(); i++) {
                Track& t = e->tr[i];
                Ext::Ahead& a = e->nx[i];
                t.q = std::move(a.q);
                t.eos = a.eos;
                t.failed = a.failed;
                t.last_us = a.last_us;
                n += t.q.size();
            }
            e->nx.clear();
            e->ahead = 0;
            e->ahead_tried = false;
            e->swaps++;
            e->cur_valid = false;
            NXLOG_I("movie", "SeekTo(0): loop restart from the samples decoded ahead (%zu queued)", n);
            lk.unlock();
            e->cv_work.notify_all();
            RET(ST_OK);
            return;
        }
    }
    e->nx.clear();
    e->ahead = 0;
    e->ahead_tried = false;
    e->seek_us = us;
    e->seek_pending = true;
    e->cur_valid = false;
    lk.unlock();
    e->cv_work.notify_all();
    lk.lock();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    while (e->seek_pending) {
        if (e->cv_data.wait_until(lk, deadline) == std::cv_status::timeout) { RET(ST_FAILED); return; }
    }
    RET(ST_OK);
}

void ext_track_for_data(::Ctx* c) {          // GetTrackIndexForAvailableData(size_t* track)
    Ext* e = ext_of(A0);
    if (!e || !A1) { RET(ST_INVALID); return; }
    std::unique_lock<std::mutex> lk(e->m);
    const int r = ext_fill(e, lk);
    if (r == ST_OK) wr64(A1, static_cast<uint64_t>(e->cur_track));
    RET(r);
}

struct Token {
    uint64_t magic;
    SamplePtr* sp;          // owned: a heap shared_ptr, released by the decoder that takes the token (or when the slot is cleared)
    int64_t ts;
    uint64_t pad;
};
static_assert(sizeof(Token) <= kSlotBytes, "token must fit a decoder input slot");

void ext_read(::Ctx* c) {                    // Read(Buffer* out): the token for the current sample goes into the decoder's input slot
    Ext* e = ext_of(A0);
    const uint64_t b = A1;
    if (!e || !b) { RET(ST_INVALID); return; }
    std::unique_lock<std::mutex> lk(e->m);
    int r = ext_fill(e, lk);
    if (r == ST_OK) {
        const uint64_t base = rd64(b + B_BASE), cap = rd64(b + B_CAP);
        Track& t = e->tr[static_cast<size_t>(e->cur_track)];
        if (base && cap >= sizeof(Token) && !t.q.empty()) {
            const Item& it = t.q.front();
            Token tok{kTokenMagic, new SamplePtr(it.s), it.us, 0};
            std::memcpy(reinterpret_cast<void*>(base), &tok, sizeof tok);
            wr32(b + B_OFFSET, 0);
            wr32(b + B_SIZE, static_cast<uint32_t>(sizeof tok));
            wr64(b + B_I64, static_cast<uint64_t>(it.us));
        } else {
            r = ST_FAILED;
        }
    }
    RET(r);
}

void ext_advance(::Ctx* c) {
    Ext* e = ext_of(A0);
    if (!e) { RET(ST_INVALID); return; }
    {
        std::lock_guard<std::mutex> lk(e->m);
        if (e->cur_valid) {
            Track& t = e->tr[static_cast<size_t>(e->cur_track)];
            if (!t.q.empty()) t.q.pop_front();
            e->cur_valid = false;
        }
    }
    e->cv_work.notify_all();
    RET(ST_OK);
}

void ext_sample_time(::Ctx* c) {             // GetSampleTime(int64* timeUs)
    Ext* e = ext_of(A0);
    if (!e || !A1) { RET(ST_INVALID); return; }
    std::unique_lock<std::mutex> lk(e->m);
    const int r = ext_fill(e, lk);
    wr64(A1, static_cast<uint64_t>(r == ST_OK ? e->cur_us : int64_t{-1}));
    RET(r);
}

// ---- the decoder ---------------------------------------------------------------------------------------------------------------
constexpr int MAX_IN = 16, MAX_OUT = 64;
enum { IN_CODEC = 0, IN_OFFERED, IN_CLIENT, IN_DECODING };
enum { OUT_FREE = 0, OUT_READY, OUT_CLIENT };
struct OutBuf {
    int state = OUT_FREE;
    SamplePtr s;
    uint32_t off = 0, len = 0;
    int64_t ts = 0;
    uint32_t flags = 0;
};
struct DecItem {
    SamplePtr s;
    uint32_t off = 0, remaining = 0;
    int64_t ts = 0;
    uint32_t flags = 0;
    int in_idx = 0;
};
struct Dec;
std::mutex g_decs_mu;
std::set<Dec*> g_decs;

struct Dec {
    Dec() { std::lock_guard<std::mutex> lk(g_decs_mu); g_decs.insert(this); }
    ~Dec() { std::lock_guard<std::mutex> lk(g_decs_mu); g_decs.erase(this); }
    Dec(const Dec&) = delete;
    Dec& operator=(const Dec&) = delete;
    std::mutex m;
    int type = 0, fmt = 0, mode = 0;
    bool is_video = false;
    int w = 0, h = 0, rate = 0, ch = 0, colorspace = 2;
    int nin = 8, nout = 16;
    bool started = false;
    unsigned char in_state[MAX_IN] = {};
    int in_list[MAX_IN] = {}, in_n = 0;
    std::deque<DecItem> dec_q;
    OutBuf out[MAX_OUT];
    int out_list[MAX_OUT] = {}, out_n = 0;
    std::unique_ptr<unsigned char[]> mem;
    uint64_t events = 0;
    uint64_t in_count = 0, copied = 0, dropped = 0;
    bool warned_small = false, warned_token = false;
};
Dec* dec_of(uint64_t guest) { return guest && rd64(guest) == dec_vtable() ? reinterpret_cast<Dec*>(rd64(guest + 8)) : nullptr; }

// ---- gauges (include/nxrt/gauge.h) ----
// Queued samples over every live extractor (kind 0 video, 1 audio): the registry lock first, then each extractor's m.
int64_t queued_samples(int kind) {
    std::lock_guard<std::mutex> lk(g_exts_mu);
    int64_t n = 0;
    for (Ext* e : g_exts) {
        std::lock_guard<std::mutex> el(e->m);
        for (const Track& t : e->tr)
            if (t.kind == kind) n += static_cast<int64_t>(t.q.size());
    }
    return n;
}
int64_t decoder_queue() {
    std::lock_guard<std::mutex> lk(g_decs_mu);
    int64_t n = 0;
    for (Dec* d : g_decs) {
        std::lock_guard<std::mutex> dl(d->m);
        n += static_cast<int64_t>(d->dec_q.size());
    }
    return n;
}
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};
GaugeHook gauge_queued_video("media.queued_video", [] { return queued_samples(0); });
GaugeHook gauge_queued_audio("media.queued_audio", [] { return queued_samples(1); });
GaugeHook gauge_decoder_queue("media.decoder_queue", [] { return decoder_queue(); });
GaugeHook gauge_extractors("media.extractors_open", [] {
    return static_cast<int64_t>(s_extractors_created.load()) - static_cast<int64_t>(s_extractors_destroyed.load());
});
GaugeHook gauge_decoders("media.decoders_open", [] {
    return static_cast<int64_t>(s_decoders_created.load()) - static_cast<int64_t>(s_decoders_destroyed.load());
});
GaugeHook gauge_video_frames("media.video_frames", [] { return static_cast<int64_t>(s_frames.load()); });
GaugeHook gauge_audio_blocks("media.audio_blocks", [] { return static_cast<int64_t>(s_audio_blocks.load()); });
GaugeHook gauge_samples_read("media.samples_read", [] { return static_cast<int64_t>(s_samples.load()); });
GaugeHook gauge_read_errors("media.read_errors", [] { return static_cast<int64_t>(s_read_errors.load()); });

void dec_release_all(Dec* d) {
    for (int i = 0; i < d->nout; i++) d->out[i] = OutBuf();
    for (int i = 0; i < d->nin; i++) {
        Token t;
        std::memcpy(&t, d->mem.get() + static_cast<size_t>(i) * kSlotBytes, sizeof t);
        if (t.magic == kTokenMagic) delete t.sp;
        std::memset(d->mem.get() + static_cast<size_t>(i) * kSlotBytes, 0, kSlotBytes);
        d->in_state[i] = IN_CODEC;
    }
    d->dec_q.clear();
    d->in_n = d->out_n = 0;
}

void dec_pump(Dec* d) {
    while (!d->dec_q.empty()) {
        int o = -1;
        for (int i = 0; i < d->nout; i++) if (d->out[i].state == OUT_FREE) { o = i; break; }
        if (o < 0) break;
        DecItem& it = d->dec_q.front();
        OutBuf& ob = d->out[o];
        ob.s = it.s;
        ob.ts = it.ts;
        ob.flags = it.flags;
        bool done = true;
        if (!d->is_video && it.s) {
            const uint32_t frame_bytes = static_cast<uint32_t>(std::max(d->ch, 1)) * 2;
            uint32_t n = std::min<uint32_t>(it.remaining, static_cast<uint32_t>(kAudioChunk) / frame_bytes * frame_bytes);
            ob.off = it.off;
            ob.len = n;
            it.off += n;
            it.remaining -= n;
            if (d->rate > 0) it.ts += static_cast<int64_t>(n / frame_bytes) * 1000000 / d->rate;
            done = it.remaining == 0;
        }
        ob.state = OUT_READY;
        d->out_list[d->out_n++] = o;
        if (done) {
            const int in = it.in_idx;
            d->dec_q.pop_front();
            d->in_state[in] = IN_OFFERED;
            d->in_list[d->in_n++] = in;
        }
    }
}

void dec_ctor(::Ctx* c) {                    // Decoder(DecoderType, DecoderOutputFormat, DecoderMode, int priority)
    Dec* d = new Dec();
    d->type = static_cast<int>(A1);
    d->fmt = static_cast<int>(A2);
    d->mode = static_cast<int>(A3);
    d->is_video = d->type >= 1 && d->type <= 3;
    d->nin = 8;
    d->nout = d->is_video ? 16 : 64;
    d->mem.reset(new unsigned char[static_cast<size_t>(MAX_IN) * kSlotBytes]());
    wr64(A0, dec_vtable());
    wr64(A0 + 8, reinterpret_cast<uint64_t>(d));
    s_decoders_created++;
    NXLOG_I("movie", "Decoder created (type %d = %s, output format %d, mode %d)", d->type,
            d->type == 1 ? "H.264" : d->type == 2 ? "VP8" : d->type == 3 ? "VP9" : d->type == 4 ? "AAC" : d->type == 5 ? "Vorbis" : "?", d->fmt, d->mode);
    if (d->is_video && d->fmt != 4) NXLOG_W("movie", "output format %d requested for video: only 4 (NV12) is produced", d->fmt);
}
void dec_dtor(::Ctx* c) {
    Dec* d = dec_of(A0);
    if (!d) return;
    wr64(A0 + 8, 0);
    wr64(A0, 0);
    {
        std::lock_guard<std::mutex> lk(d->m);
        dec_release_all(d);
    }
    NXLOG_I("movie", "Decoder(%s) destroyed: %llu inputs, %llu buffers copied to the game, %llu refused", d->is_video ? "video" : "audio",
            static_cast<unsigned long long>(d->in_count), static_cast<unsigned long long>(d->copied), static_cast<unsigned long long>(d->dropped));
    delete d;
    s_decoders_destroyed++;
}

void dec_configure(::Ctx* c) {               // Configure(MediaData* format): the track configuration the extractor made
    Dec* d = dec_of(A0);
    MD* m = md_of(A1);
    if (!d || !m) { RET(ST_INVALID); return; }
    int32_t v = 0;
    if (m->get_i32("width", &v)) d->w = v;
    if (m->get_i32("height", &v)) d->h = v;
    if (m->get_i32("sample-rate", &v)) d->rate = v;
    if (m->get_i32("channel-count", &v)) d->ch = v;
    if (m->get_i32("nn-colorspace", &v)) d->colorspace = v;
    NXLOG_I("movie", "Decoder(%s) configured: %dx%d, %d Hz, %d ch", d->is_video ? "video" : "audio", d->w, d->h, d->rate, d->ch);
    RET(ST_OK);
}
void dec_register_events(::Ctx* c) {         // DecoderEvents*: four nn::os::EventType* (input, output, format changed, error) or null
    Dec* d = dec_of(A0);
    if (!d) { RET(ST_INVALID); return; }
    d->events = A1;
    if (A1 && (rd64(A1) || rd64(A1 + 8))) NXLOG_I("movie", "Decoder(%s): client events registered (signalled on the vtable path)", d->is_video ? "video" : "audio");
    RET(ST_OK);
}

void fill_format(Dec* d, MD* m) {
    m->e.clear();
    if (d->is_video) {
        const int w = d->w, h = d->h;
        m->set_str("mime", "video/raw");
        m->set_i32("width", w); m->set_i32("height", h);
        m->set_i32("stride", w); m->set_i32("slice-height", h);
        m->set_i32("crop-width", w); m->set_i32("crop-height", h);
        m->set_rect("crop", 0, 0, w - 1, h - 1);
        m->set_i32("color-format", 21);                  // OMX_COLOR_FormatYUV420SemiPlanar = NV12
        m->set_i32("nv12-width", w); m->set_i32("nv12-height", h);
        m->set_i32("nv12-y-stride", w); m->set_i32("nv12-uv-stride", w);
        m->set_i32("nv12-y-offset", 0); m->set_i32("nv12-uv-offset", w * h);
        m->set_i32("nv12-colorspace", d->colorspace);
    } else {
        m->set_str("mime", "audio/raw");
        m->set_i32("sample-rate", d->rate); m->set_i32("channel-count", d->ch);
        m->set_i32("pcm-encoding", 2);                   // 16 bit
    }
}
void dec_get_format(::Ctx* c) {
    Dec* d = dec_of(A0);
    MD* m = md_of(A1);
    if (!d || !m) { RET(ST_INVALID); return; }
    fill_format(d, m);
    RET(ST_OK);
}
void dec_output_size(::Ctx* c) {             // GetOutputBufferSize(int* size, int* alignment)
    Dec* d = dec_of(A0);
    if (!d || !A1) { RET(ST_INVALID); return; }
    const int size = d->is_video ? d->w * d->h + 2 * ((d->w + 1) / 2) * ((d->h + 1) / 2) : kAudioChunk;
    wr32(A1, static_cast<uint32_t>(size));
    if (A2) wr32(A2, 16);
    RET(ST_OK);
}

void dec_start(::Ctx* c) {
    Dec* d = dec_of(A0);
    if (!d) { RET(ST_INVALID); return; }
    std::lock_guard<std::mutex> lk(d->m);
    if (!d->started) {
        dec_release_all(d);
        for (int i = 0; i < d->nin; i++) { d->in_state[i] = IN_OFFERED; d->in_list[i] = i; }
        d->in_n = d->nin;
        d->started = true;
    }
    RET(ST_OK);
}
void dec_stop(::Ctx* c) {
    Dec* d = dec_of(A0);
    if (!d) { RET(ST_INVALID); return; }
    std::lock_guard<std::mutex> lk(d->m);
    dec_release_all(d);
    d->started = false;
    RET(ST_OK);
}

void dec_acquire_input(::Ctx* c) {           // AcquireInputBufferIndex(int* index, int* moreAvailable)
    Dec* d = dec_of(A0);
    if (!d || !A1 || !A2) { RET(ST_INVALID); return; }
    std::lock_guard<std::mutex> lk(d->m);
    int found = -1, more = 0;
    if (d->in_n > 0) {
        found = d->in_list[0];
        std::memmove(d->in_list, d->in_list + 1, static_cast<size_t>(d->in_n - 1) * sizeof(int));
        more = --d->in_n;
        d->in_state[found] = IN_CLIENT;
    }
    wr32(A1, static_cast<uint32_t>(found));
    wr32(A2, static_cast<uint32_t>(more));
    RET(found >= 0 ? ST_OK : ST_NO_INPUT);
}
void dec_get_input(::Ctx* c) {               // GetInputBuffer(int index, Buffer* out)
    Dec* d = dec_of(A0);
    const int i = static_cast<int>(A1);
    const uint64_t b = A2;
    if (!d || !b || i < 0 || i >= d->nin) { RET(ST_INVALID); return; }
    wr64(b + B_BASE, reinterpret_cast<uint64_t>(d->mem.get() + static_cast<size_t>(i) * kSlotBytes));
    wr64(b + B_CAP, kSlotBytes);
    wr32(b + B_OFFSET, 0);
    wr32(b + B_SIZE, 0);
    RET(ST_OK);
}

void dec_send_input(::Ctx* c) {              // SendInputBufferForDecode(int index, int offset, int size, int64 timeUs, unsigned flags)
    Dec* d = dec_of(A0);
    const int i = static_cast<int>(A1), size = static_cast<int>(A3);
    int64_t ts = static_cast<int64_t>(A4);
    const uint32_t flags = static_cast<uint32_t>(A5);
    if (!d || i < 0 || i >= d->nin) { RET(ST_INVALID); return; }
    std::lock_guard<std::mutex> lk(d->m);
    SamplePtr sample;
    Token t;
    std::memcpy(&t, d->mem.get() + static_cast<size_t>(i) * kSlotBytes, sizeof t);
    std::memset(d->mem.get() + static_cast<size_t>(i) * kSlotBytes, 0, sizeof t);
    if (t.magic == kTokenMagic && t.sp) {
        sample = *t.sp;
        delete t.sp;
        if (ts <= 0) ts = t.ts;
    } else if (!(flags & FLAG_EOS) && !d->warned_token) {
        d->warned_token = true;
        NXLOG_W("movie", "Decoder(%s): input buffer %d holds no sample token (size %d): dropped", d->is_video ? "video" : "audio", i, size);
    }
    if (d->in_state[i] != IN_CLIENT) {           // an index voided by Stop: the codec refuses it
        d->dropped++;
        RET(ST_INVALID);
        return;
    }
    d->in_count++;
    const bool empty_audio = sample && !d->is_video && !(flags & FLAG_EOS) && sample->data.empty();
    if ((!sample || empty_audio) && !(flags & FLAG_EOS)) {
        d->dropped++;
        d->in_state[i] = IN_OFFERED;
        d->in_list[d->in_n++] = i;
        RET(ST_OK);
        return;
    }
    DecItem it;
    it.s = sample;
    it.ts = ts;
    it.flags = flags & FLAG_EOS;
    it.in_idx = i;
    if (sample && !d->is_video) {
        it.off = 0;
        it.remaining = static_cast<uint32_t>(sample->data.size());
    }
    if (g_log) NXLOG_I("movie", "SendInput(%s) idx %d t=%.3f flags %x", d->is_video ? "video" : "audio", i, ts / 1e6, flags);
    d->in_state[i] = IN_DECODING;
    d->dec_q.push_back(std::move(it));
    dec_pump(d);
    RET(ST_OK);
}

void dec_acquire_output(::Ctx* c) {          // AcquireOutputBufferIndex(int* index, int64* timeUs, unsigned* flags, int* moreAvailable)
    Dec* d = dec_of(A0);
    if (!d || !A1 || !A2 || !A3 || !A4) { RET(ST_INVALID); return; }
    std::lock_guard<std::mutex> lk(d->m);
    int r = ST_NO_OUTPUT, idx = -1, more = 0;
    int64_t ts = 0;
    uint32_t flags = 0;
    if (d->out_n > 0) {
        idx = d->out_list[0];
        std::memmove(d->out_list, d->out_list + 1, static_cast<size_t>(d->out_n - 1) * sizeof(int));
        more = --d->out_n;
        OutBuf& s = d->out[idx];
        s.state = OUT_CLIENT;
        ts = s.ts;
        flags = s.flags;
        r = ST_OK;
    }
    wr32(A1, static_cast<uint32_t>(idx));
    wr64(A2, static_cast<uint64_t>(ts));
    wr32(A3, flags);
    wr32(A4, static_cast<uint32_t>(more));
    RET(r);
}

// The sample's bytes into the game's buffer: a video frame is NV12 at the size of the track (see SourceSample), audio is a slice of the PCM.
size_t copy_out(Dec* d, const OutBuf& s, unsigned char* dst, size_t cap) {
    if (!s.s) return 0;
    const std::vector<uint8_t>& data = s.s->data;
    size_t written = 0;
    if (d->is_video) {
        const size_t w = static_cast<size_t>(d->w), h = static_cast<size_t>(d->h);
        const size_t need = w * h + w * ((h + 1) / 2);
        if (data.size() < need) return 0;
        if (cap < need) {
            if (!d->warned_small) { d->warned_small = true; NXLOG_W("movie", "the game's buffer of %zu bytes is smaller than a %dx%d NV12 frame (%zu)", cap, d->w, d->h, need); }
        } else {
            std::memcpy(dst, data.data(), need);
            written = need;
        }
    } else {
        const size_t off = std::min<size_t>(s.off, data.size());
        const size_t n = std::min<size_t>(s.len, data.size() - off);
        written = std::min(n, cap);
        std::memcpy(dst, data.data() + off, written);
        if (n > cap && !d->warned_small) { d->warned_small = true; NXLOG_W("movie", "the game's audio buffer of %zu bytes is smaller than a block (%zu)", cap, n); }
    }
    return written;
}

void dec_get_output(::Ctx* c) {              // GetOutputBuffer(int index, Buffer* out, MediaData* format)
    Dec* d = dec_of(A0);
    const int i = static_cast<int>(A1);
    const uint64_t b = A2;
    if (!d || !b || i < 0 || i >= d->nout) { RET(ST_INVALID); return; }
    if (MD* m = md_of(A3)) fill_format(d, m);
    std::lock_guard<std::mutex> lk(d->m);
    OutBuf& s = d->out[i];
    if (s.state != OUT_CLIENT) { RET(ST_INVALID); return; }
    const uint64_t base = rd64(b + B_BASE), cap = rd64(b + B_CAP);
    const size_t n = base ? copy_out(d, s, reinterpret_cast<unsigned char*>(base), static_cast<size_t>(cap)) : 0;
    wr32(b + B_OFFSET, 0);
    wr32(b + B_SIZE, static_cast<uint32_t>(n));
    wr64(b + B_I64, static_cast<uint64_t>(s.ts));
    wr32(b + B_FLAGS, s.flags);
    if (n) {
        d->copied++;
        if (d->is_video) s_frames++;
        else { s_audio_blocks++; s_audio_bytes += n; }
        if (d->copied == 1) NXLOG_I("movie", "Decoder(%s): first %s copied to the game (%zu bytes, t=%.3f s)", d->is_video ? "video" : "audio", d->is_video ? "frame" : "block", n, s.ts / 1e6);
    }
    RET(ST_OK);
}
void dec_release_output(::Ctx* c) {          // ReleaseOutputBufferIndex(int index)
    Dec* d = dec_of(A0);
    const int i = static_cast<int>(A1);
    if (!d || i < 0 || i >= d->nout) { RET(ST_INVALID); return; }
    std::lock_guard<std::mutex> lk(d->m);
    OutBuf& s = d->out[i];
    if (s.state == OUT_CLIENT) {
        s = OutBuf();
        dec_pump(d);
    }
    RET(ST_OK);
}

// ---- vtables ---------------------------------------------------------------------------------------------------------------
// Older SDKs (SDK 9 subsdk0, read off its vtables) construct the Extractor with one argument, Extractor(ContainerType), and call every
// member through the vtable at +0 of the object; the arguments are those of the exported wrappers. Slots of the vtables, in order:
//   Extractor: ~Extractor x2, SetDataSource(path) (read from the RomFS folder), SetDataSource(data, size), GetTrackCount, GetTrackConfiguration, SelectTrack,
//     UnselectTrack, SeekTo, GetTrackIndexForAvailableData, Read, Advance, GetSampleTime, GetSampleMetaData, GetBufferedRange,
//     SetDataSource(ClientStreamReader*, path)
//   Decoder: ~Decoder x2, Configure, RegisterClientEvents, GetOutputFormat, Start, Stop, Reset, Flush, AcquireInputBufferIndex,
//     GetInputBuffer, SendInputBufferForDecode, AcquireOutputBufferIndex, GetOutputBuffer(index, buffer), GetOutputBufferSize,
//     ReleaseOutputBufferIndex, GetLastError, GetOutputBuffer(index, buffer, format), GetOutputBufferFormat
// Members the wrapper API has no handler for answer ST_UNSUPPORTED (data sources other than the game's reader, the metadata and range
// queries) or ST_OK (Reset, Flush, GetLastError, UnselectTrack: nothing to do for the host decoder).
void vt_unsupported(::Ctx* c) { RET(ST_UNSUPPORTED); }
void vt_ok(::Ctx* c) { RET(ST_OK); }
void dec_get_output_no_format(::Ctx* c) {    // GetOutputBuffer(int index, Buffer* out)
    c->x[3] = 0;
    dec_get_output(c);
}
void dec_get_output_format(::Ctx* c) {       // GetOutputBufferFormat(int index, MediaData* format)
    c->x[1] = c->x[2];
    dec_get_format(c);
}

// Decoder client events (older SDKs): DecoderEvents is four nn::os::EventType pointers, signalled by the library when an input buffer
// is free (0), an output buffer is ready (1), the output format changed (2) or an error happened (3). The game's decoder thread waits
// for them (WaitAny) before it asks for buffers, so the vtable path signals them after the calls that change the answer, outside the
// decoder lock, through the SDK's own nn::os::SignalEvent. A game that passes null events (the wrapper API) is not affected.
enum { EV_INPUT = 0, EV_OUTPUT = 1, EV_FORMAT = 2 };
void dec_signal(::Ctx* c, uint64_t obj, int which) {
    Dec* d = dec_of(obj);
    if (!d || !d->events) return;
    const uint64_t ev = rd64(d->events + 8u * static_cast<uint64_t>(which));
    if (!ev) return;
    static const uint64_t signal = module_find_symbol("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    if (!signal) {
        static std::once_flag once;
        std::call_once(once, [] { NXLOG_W("movie", "nn::os::SignalEvent not found: decoder events are not signalled"); });
        return;
    }
    gcall(c, signal, ev, 0, 0, 0, 0);
}
void ev_dec_start(::Ctx* c) {
    const uint64_t obj = A0;
    dec_start(c);
    if (static_cast<int>(c->x[0]) != ST_OK) return;
    if (Dec* d = dec_of(obj); d && d->is_video) dec_signal(c, obj, EV_FORMAT);
    dec_signal(c, obj, EV_INPUT);
    c->x[0] = ST_OK;
}
void ev_dec_send_input(::Ctx* c) {
    const uint64_t obj = A0;
    dec_send_input(c);
    const uint64_t r = c->x[0];
    dec_signal(c, obj, EV_INPUT);
    dec_signal(c, obj, EV_OUTPUT);
    c->x[0] = r;
}
void ev_dec_acquire_output(::Ctx* c) {
    const uint64_t obj = A0;
    dec_acquire_output(c);
    const uint64_t r = c->x[0];
    if (static_cast<int>(r) == ST_OK) dec_signal(c, obj, EV_OUTPUT);   // there may be more: the game takes one per wake
    c->x[0] = r;
}
void ev_dec_release_output(::Ctx* c) {
    const uint64_t obj = A0;
    dec_release_output(c);
    const uint64_t r = c->x[0];
    dec_signal(c, obj, EV_OUTPUT);
    dec_signal(c, obj, EV_INPUT);
    c->x[0] = r;
}

uint64_t object_vtable(const char* cls, const std::initializer_list<HleFn>& slots) {
    const uint64_t t = mem::alloc_kernel(slots.size() * 8, 16);
    uint64_t i = 0;
    for (HleFn f : slots) {
        const std::string name = std::string("movie::") + cls + " vtable slot " + std::to_string(i);
        wr64(t + 8 * i++, hle_register(name.c_str(), f));
    }
    return t;
}
uint64_t ext_vtable() {
    static const uint64_t t = object_vtable("Extractor", {ext_dtor, ext_dtor, ext_set_source_path, vt_unsupported, ext_track_count,
        ext_track_config, ext_select, vt_ok, ext_seek, ext_track_for_data, ext_read, ext_advance, ext_sample_time, vt_unsupported,
        vt_unsupported, ext_set_source});
    return t;
}
uint64_t dec_vtable() {
    static const uint64_t t = object_vtable("Decoder", {dec_dtor, dec_dtor, dec_configure, dec_register_events, dec_get_format,
        ev_dec_start, dec_stop, vt_ok, vt_ok, dec_acquire_input, dec_get_input, ev_dec_send_input, ev_dec_acquire_output,
        dec_get_output_no_format, dec_output_size, ev_dec_release_output, vt_ok, dec_get_output, dec_get_output_format});
    return t;
}

// ---- free functions ------------------------------------------------------------------------------------------------------------
void movie_set_allocator(::Ctx* c) { NXLOG_D("movie", "SetAllocator(alloc %llx, free %llx)", static_cast<unsigned long long>(A0), static_cast<unsigned long long>(A1)); }
void movie_set_core_mask(::Ctx*) {}
void movie_thread_priority(::Ctx* c) { RET(0); }

struct Import { const char* name; HleFn fn; };
const Import kImports[] = {
    {"_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", movie_set_allocator},
    {"_ZN5movie11SetCoreMaskEm", movie_set_core_mask},
    {"_ZN5movie17GetThreadPriorityEv", movie_thread_priority},
    {"_ZN5movie6BufferC1Ev", buf_ctor},
    {"_ZN5movie6BufferC1EPvm", buf_ctor_mem},
    {"_ZN5movie6BufferD1Ev", buf_dtor},
    {"_ZNK5movie6Buffer4BaseEv", buf_base},
    {"_ZNK5movie6Buffer4SizeEv", buf_size},
    {"_ZNK5movie6Buffer6OffsetEv", buf_offset},
    {"_ZN5movie9MediaDataC1Ev", mdata_ctor},
    {"_ZN5movie9MediaDataC1ERKS0_", mdata_copy},
    {"_ZN5movie9MediaDataD1Ev", mdata_dtor},
    {"_ZN5movie9MediaData5ClearEv", mdata_clear},
    {"_ZNK5movie9MediaData9FindInt32EPKcPi", mdata_find_i32},
    {"_ZN5movie9MediaData8SetInt32EPKci", mdata_set_i32},
    {"_ZNK5movie9MediaData8FindRectEPKcPiS3_S3_S3_", mdata_find_rect},
    {"_ZNK5movie9MediaData9FindInt64EPKcPl", mdata_find_i64},
    {"_ZNK5movie9MediaData10FindStringEPKcPS2_", mdata_find_str},
    {"_ZN5movie9ExtractorC1ENS_13ContainerTypeENS_9CacheSizeE", ext_ctor},
    {"_ZN5movie9ExtractorD1Ev", ext_dtor},
    {"_ZN5movie9ExtractorC1ENS_13ContainerTypeE", ext_ctor},
    {"_ZN5movie9Extractor13SetDataSourceEPNS_18ClientStreamReaderEPKc", ext_set_source},
    {"_ZN5movie9Extractor13GetTrackCountEPi", ext_track_count},
    {"_ZN5movie9Extractor21GetTrackConfigurationEiPNS_9MediaDataE", ext_track_config},
    {"_ZN5movie9Extractor11SelectTrackEi", ext_select},
    {"_ZN5movie9Extractor29GetTrackIndexForAvailableDataEPm", ext_track_for_data},
    {"_ZN5movie9Extractor13GetSampleTimeEPl", ext_sample_time},
    {"_ZN5movie9Extractor4ReadEPNS_6BufferE", ext_read},
    {"_ZN5movie9Extractor7AdvanceEv", ext_advance},
    {"_ZN5movie9Extractor6SeekToEl", ext_seek},
    {"_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", dec_ctor},
    {"_ZN5movie7DecoderD1Ev", dec_dtor},
    {"_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", dec_register_events},
    {"_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", dec_configure},
    {"_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_", dec_output_size},
    {"_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE", dec_get_format},
    {"_ZN5movie7Decoder5StartEv", dec_start},
    {"_ZN5movie7Decoder4StopEv", dec_stop},
    {"_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", dec_acquire_input},
    {"_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE", dec_get_input},
    {"_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", dec_send_input},
    {"_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_", dec_acquire_output},
    {"_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", dec_get_output},
    {"_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", dec_release_output},
};
constexpr size_t kImportCount = sizeof(kImports) / sizeof(kImports[0]);

const char* g_names[kImportCount];

}

const char* const* movie_import_names(size_t* count) {
    for (size_t i = 0; i < kImportCount; i++) g_names[i] = kImports[i].name;
    if (count) *count = kImportCount;
    return g_names;
}

bool install_movie_hle() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        const std::string mode = config_get_str("media", "movie", "on");
        if (mode == "off") {
            NXLOG_W("movie", "[media] movie = off: the lifted subsdk0 library handles movie::");
            return;
        }
        const std::string backend = config_get_str("media", "backend", "ffmpeg");
        g_use_mf = backend == "mf";
        if (g_use_mf && !make_mf_source()) {
            NXLOG_W("movie", "[media] backend = mf: Media Foundation is not available on this system, using FFmpeg");
            g_use_mf = false;
        }
        g_log = config_get_bool("media", "movie_log", false);
        g_lookahead = config_get_bool("media", "movie_lookahead", true);
        g_preroll = config_get_bool("media", "movie_preroll", true);
        g_async_close = config_get_bool("media", "movie_async_close", true);
        for (const Import& im : kImports) {
            hle_register(im.name, im.fn);
            module_override_symbol(im.name);
        }
        ok = true;
        NXLOG_I("movie", "movie:: on %s: %zu imports routed to the host", g_use_mf ? "Media Foundation" : "FFmpeg", kImportCount);
    });
    return ok;
}

MovieStats movie_stats() {
    MovieStats s;
    s.extractors_created = s_extractors_created;
    s.extractors_destroyed = s_extractors_destroyed;
    s.decoders_created = s_decoders_created;
    s.decoders_destroyed = s_decoders_destroyed;
    s.video_frames_delivered = s_frames;
    s.audio_blocks_delivered = s_audio_blocks;
    s.audio_bytes_delivered = s_audio_bytes;
    s.samples_read = s_samples;
    s.read_errors = s_read_errors;
    return s;
}

}
