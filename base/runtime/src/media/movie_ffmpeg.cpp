// SPDX-License-Identifier: GPL-3.0-or-later
// The FFmpeg backend of the movie:: library (include/nxrt/movie_source.h): libavformat demuxes the movie the game handed over (read into memory
// by src/media/movie_hle.cpp), libavcodec decodes it (H.264, VP8, VP9, AAC, Opus, Vorbis ...), libswscale makes NV12 of the pictures and
// libswresample 16-bit PCM of the sound. The same code on Windows and Linux; no OS call in here.
//
// Written against the API of FFmpeg 4.4 (libavcodec 58, the Ubuntu 22.04 package) up to 9.0: the channel layout API changed in 5.1 and is the one
// place that needs `#if` (nxff_*). Timestamps: libavformat applies the edit lists of an MP4 (first frame at 0, AAC priming cut), so the
// presentation time of a packet is used as it comes, minus the start time of the file (what the ffmpeg tool does).
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/movie_source.h"

#define NXFF_NEW_LAYOUT (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100))

namespace nxrt::media {
namespace {

constexpr AVRational kUs = {1, 1000000};

// ---- the channel layout API of FFmpeg before and after 5.1 ----------------------------------------------------------------------
inline int par_channels(const AVCodecParameters* p) {
#if NXFF_NEW_LAYOUT
    return p->ch_layout.nb_channels;
#else
    return p->channels;
#endif
}
inline int frame_channels(const AVFrame* f) {
#if NXFF_NEW_LAYOUT
    return f->ch_layout.nb_channels;
#else
    return f->channels;
#endif
}

// A resampler from what the decoder delivers to interleaved 16-bit PCM at the track's rate and channel count. Made on the first frame (its
// format is the truth). The channel order of the decoder is kept; a different count than the track's gets the default layout.
struct Resampler {
    SwrContext* swr = nullptr;
    int in_fmt = -1, in_rate = 0, in_ch = 0;
    ~Resampler() { swr_free(&swr); }

    bool ensure(const AVFrame* f, int out_rate, int out_ch) {
        const int ch = frame_channels(f);
        if (swr && in_fmt == f->format && in_rate == f->sample_rate && in_ch == ch) return true;
        swr_free(&swr);
        in_fmt = f->format; in_rate = f->sample_rate; in_ch = ch;
#if NXFF_NEW_LAYOUT
        AVChannelLayout in_l = {}, out_l = {};
        if (av_channel_layout_copy(&in_l, &f->ch_layout) < 0) return false;
        if (in_l.order == AV_CHANNEL_ORDER_UNSPEC) { av_channel_layout_uninit(&in_l); av_channel_layout_default(&in_l, ch); }
        if (ch == out_ch) av_channel_layout_copy(&out_l, &in_l); else av_channel_layout_default(&out_l, out_ch);
        const int r = swr_alloc_set_opts2(&swr, &out_l, AV_SAMPLE_FMT_S16, out_rate, &in_l, static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&in_l);
        av_channel_layout_uninit(&out_l);
        if (r < 0 || !swr) return false;
#else
        const uint64_t in_l = f->channel_layout ? f->channel_layout : static_cast<uint64_t>(av_get_default_channel_layout(ch));
        const uint64_t out_l = ch == out_ch ? in_l : static_cast<uint64_t>(av_get_default_channel_layout(out_ch));
        swr = swr_alloc_set_opts(nullptr, static_cast<int64_t>(out_l), AV_SAMPLE_FMT_S16, out_rate, static_cast<int64_t>(in_l), static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr);
        if (!swr) return false;
#endif
        if (swr_init(swr) < 0) { swr_free(&swr); return false; }
        return true;
    }
};

const char* video_mime(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return "video/avc";
        case AV_CODEC_ID_HEVC: return "video/hevc";
        case AV_CODEC_ID_VP9: return "video/x-vnd.on2.vp9";
        case AV_CODEC_ID_VP8: return "video/x-vnd.on2.vp8";
        case AV_CODEC_ID_AV1: return "video/av01";
        default: return "video/unknown";
    }
}
const char* audio_mime(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_AAC: return "audio/mp4a-latm";
        case AV_CODEC_ID_OPUS: return "audio/opus";
        case AV_CODEC_ID_VORBIS: return "audio/vorbis";
        case AV_CODEC_ID_MP3: return "audio/mpeg";
        default: return "audio/unknown";
    }
}

// ---- the movie in memory as libavformat's custom I/O ------------------------------------------------------------------------------
struct MemIo {
    const uint8_t* data = nullptr;
    size_t size = 0, pos = 0;
};
int mem_read(void* opaque, uint8_t* buf, int n) {
    MemIo* io = static_cast<MemIo*>(opaque);
    if (io->pos >= io->size) return AVERROR_EOF;
    const size_t k = std::min<size_t>(static_cast<size_t>(n), io->size - io->pos);
    std::memcpy(buf, io->data + io->pos, k);
    io->pos += k;
    return static_cast<int>(k);
}
int64_t mem_seek(void* opaque, int64_t off, int whence) {
    MemIo* io = static_cast<MemIo*>(opaque);
    if (whence & AVSEEK_SIZE) return static_cast<int64_t>(io->size);
    whence &= ~AVSEEK_FORCE;
    int64_t p;
    switch (whence) {
        case SEEK_SET: p = off; break;
        case SEEK_CUR: p = static_cast<int64_t>(io->pos) + off; break;
        case SEEK_END: p = static_cast<int64_t>(io->size) + off; break;
        default: return AVERROR(EINVAL);
    }
    if (p < 0) return AVERROR(EINVAL);
    io->pos = static_cast<size_t>(std::min<int64_t>(p, static_cast<int64_t>(io->size)));
    return p;
}

void av_log_to_nxlog(void* avcl, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_ERROR) return;   // warnings of the demuxers and decoders about damaged input are noise here
    static thread_local int print_prefix = 1;
    char line[512];
    av_log_format_line(avcl, level, fmt, vl, line, sizeof line, &print_prefix);
    size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n) NXLOG_W("ffmpeg", "%s", line);
}

struct Packet {
    AVPacket* p = av_packet_alloc();
    ~Packet() { av_packet_free(&p); }
};
struct Frame {
    AVFrame* f = av_frame_alloc();
    ~Frame() { av_frame_free(&f); }
};

class FfmpegSource final : public MovieSource {
public:
    FfmpegSource() {
        static bool once = [] { av_log_set_callback(av_log_to_nxlog); return true; }();
        (void)once;
        const int64_t t = config_get_int("media", "ffmpeg_threads", 0);
        threads_ = t > 0 ? static_cast<int>(std::min<int64_t>(t, 16)) : static_cast<int>(std::clamp<unsigned>(std::thread::hardware_concurrency(), 1u, 4u));
    }
    ~FfmpegSource() override {
        for (Track& t : tr_) {
            avcodec_free_context(&t.dec);
            sws_freeContext(t.sws);
        }
        avformat_close_input(&fmt_);
        if (io_ctx_) {
            av_freep(&io_ctx_->buffer);
            avio_context_free(&io_ctx_);
        }
    }

    bool open(const uint8_t* data, size_t size, int) override {
        io_.data = data;
        io_.size = size;
        constexpr int kIoBuf = 1 << 16;
        uint8_t* buf = static_cast<uint8_t*>(av_malloc(kIoBuf));
        if (!buf) return false;
        io_ctx_ = avio_alloc_context(buf, kIoBuf, 0, &io_, mem_read, nullptr, mem_seek);
        if (!io_ctx_) { av_free(buf); return false; }
        fmt_ = avformat_alloc_context();
        if (!fmt_) return false;
        fmt_->pb = io_ctx_;
        fmt_->flags |= AVFMT_FLAG_CUSTOM_IO;
        int r = avformat_open_input(&fmt_, "", nullptr, nullptr);   // frees fmt_ on failure
        if (r < 0) {
            char e[128];
            av_strerror(r, e, sizeof e);
            NXLOG_W("movie", "avformat_open_input: %s", e);
            fmt_ = nullptr;
            return false;
        }
        r = avformat_find_stream_info(fmt_, nullptr);
        if (r < 0) {
            char e[128];
            av_strerror(r, e, sizeof e);
            NXLOG_W("movie", "avformat_find_stream_info: %s", e);
            return false;
        }
        duration_us_ = fmt_->duration != AV_NOPTS_VALUE ? fmt_->duration : 0;
        base_us_ = fmt_->start_time != AV_NOPTS_VALUE && fmt_->start_time > 0 ? fmt_->start_time : 0;
        // the game's order: video and audio streams in file order, then the rest
        for (int pass = 0; pass < 2; pass++) {
            for (unsigned i = 0; i < fmt_->nb_streams; i++) {
                AVStream* st = fmt_->streams[i];
                const AVCodecParameters* par = st->codecpar;
                const bool pic = (st->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
                const bool av = !pic && (par->codec_type == AVMEDIA_TYPE_VIDEO || par->codec_type == AVMEDIA_TYPE_AUDIO);
                if ((pass == 0) != av) continue;
                Track t;
                t.st = st;
                SourceTrack& s = t.info;
                st->discard = AVDISCARD_ALL;   // until the game selects it
                if (av && par->codec_type == AVMEDIA_TYPE_VIDEO) {
                    s.kind = 0;
                    s.mime = video_mime(par->codec_id);
                    s.w = par->width;
                    s.h = par->height;
                    s.bitrate = static_cast<int>(par->bit_rate);
                    const AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
                    s.fps = fr.den ? av_q2d(fr) : 0.0;
                    // the game's colour conversion: 0 BT.601 limited, 1 BT.601 full, 2 BT.709 limited, 3 BT.709 full (unknown: BT.709 limited)
                    const bool bt601 = par->color_space == AVCOL_SPC_BT470BG || par->color_space == AVCOL_SPC_SMPTE170M || par->color_space == AVCOL_SPC_FCC;
                    const bool full = par->color_range == AVCOL_RANGE_JPEG || par->format == AV_PIX_FMT_YUVJ420P || par->format == AV_PIX_FMT_YUVJ422P || par->format == AV_PIX_FMT_YUVJ444P;
                    s.colorspace = bt601 ? (full ? 1 : 0) : (full ? 3 : 2);
                } else if (av) {
                    s.kind = 1;
                    s.mime = audio_mime(par->codec_id);
                    s.rate = par->sample_rate;
                    s.ch = par_channels(par);
                    s.bitrate = static_cast<int>(par->bit_rate);
                } else {
                    s.kind = 2;
                    s.mime = "application/octet-stream";
                }
                tr_.push_back(std::move(t));
            }
        }
        for (const Track& t : tr_) pub_.push_back(t.info);
        return !tr_.empty();
    }

    int64_t duration_us() const override { return duration_us_; }
    const std::vector<SourceTrack>& tracks() const override { return pub_; }

    SelectResult select(int i, bool on, SourceTrack* updated) override {
        if (i < 0 || i >= static_cast<int>(tr_.size())) return SelectResult::Failed;
        Track& t = tr_[static_cast<size_t>(i)];
        if (t.info.kind == 2) { if (updated) *updated = t.info; return SelectResult::Ok; }
        if (!on) {
            t.selected = false;
            t.st->discard = AVDISCARD_ALL;
            t.ready.clear();
            return SelectResult::Ok;
        }
        if (!t.dec) {
            const AVCodec* codec = avcodec_find_decoder(t.st->codecpar->codec_id);
            if (!codec) { NXLOG_W("movie", "track %d: FFmpeg has no decoder for codec %s", i, avcodec_get_name(t.st->codecpar->codec_id)); return SelectResult::Unsupported; }
            AVCodecContext* ctx = avcodec_alloc_context3(codec);
            if (!ctx) return SelectResult::Failed;
            if (avcodec_parameters_to_context(ctx, t.st->codecpar) < 0) { avcodec_free_context(&ctx); return SelectResult::Failed; }
            ctx->pkt_timebase = t.st->time_base;
            if (t.info.kind == 0) { ctx->thread_count = threads_; ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE; }
            const int r = avcodec_open2(ctx, codec, nullptr);
            if (r < 0) {
                char e[128];
                av_strerror(r, e, sizeof e);
                NXLOG_W("movie", "track %d: cannot open the %s decoder: %s", i, codec->name, e);
                avcodec_free_context(&ctx);
                return SelectResult::Unsupported;
            }
            t.dec = ctx;
            if (t.info.kind == 0) {
                if (ctx->width > 0 && ctx->height > 0) { t.info.w = ctx->width; t.info.h = ctx->height; }
            } else {
                if (ctx->sample_rate > 0) t.info.rate = ctx->sample_rate;
#if NXFF_NEW_LAYOUT
                if (ctx->ch_layout.nb_channels > 0) t.info.ch = ctx->ch_layout.nb_channels;
#else
                if (ctx->channels > 0) t.info.ch = ctx->channels;
#endif
            }
            NXLOG_I("movie", "track %d decodes with %s%s", i, codec->name, t.info.kind == 0 ? "" : "");
        }
        t.selected = true;
        t.drained = false;
        t.ready.clear();
        t.st->discard = AVDISCARD_DEFAULT;
        if (updated) *updated = t.info;
        return SelectResult::Ok;
    }

    bool seek(int64_t us) override {
        us = std::max<int64_t>(us, 0);
        const int64_t ts = us + base_us_;
        int r = av_seek_frame(fmt_, -1, ts, AVSEEK_FLAG_BACKWARD);
        if (r < 0) r = avformat_seek_file(fmt_, -1, INT64_MIN, ts, INT64_MAX, 0);   // nothing at or before the target: take the nearest
        for (Track& t : tr_) {
            if (t.dec) avcodec_flush_buffers(t.dec);
            t.ready.clear();
            t.drained = false;
            t.eof_sent = false;
            t.next_audio_us = INT64_MIN;
        }
        eof_ = false;
        skip_before_us_ = us;
        return r >= 0;
    }

    ReadStatus read(int track, SourceSample* out) override {
        if (track < 0 || track >= static_cast<int>(tr_.size())) return ReadStatus::Error;
        Track& t = tr_[static_cast<size_t>(track)];
        if (!t.selected || !t.dec) return ReadStatus::Error;
        for (;;) {
            if (!t.ready.empty()) {
                *out = std::move(t.ready.front());
                t.ready.pop_front();
                return ReadStatus::Sample;
            }
            if (t.drained) return ReadStatus::End;
            if (!pump()) return ReadStatus::Error;
        }
    }

private:
    struct Track {
        AVStream* st = nullptr;
        AVCodecContext* dec = nullptr;
        SourceTrack info;
        bool selected = false, eof_sent = false, drained = false;
        std::deque<SourceSample> ready;
        SwsContext* sws = nullptr;
        Resampler rs;
        int64_t next_audio_us = INT64_MIN;
    };

    // Reads one packet and decodes it into its track's queue; at the end of the file flushes every decoder. False on an unrecoverable error.
    bool pump() {
        if (!eof_) {
            Packet pk;
            const int r = av_read_frame(fmt_, pk.p);
            if (r >= 0) {
                for (Track& t : tr_) {
                    if (t.st->index != pk.p->stream_index) continue;
                    if (t.selected && t.dec) decode(t, pk.p);
                    break;
                }
                return true;
            }
            if (r != AVERROR_EOF) {
                char e[128];
                av_strerror(r, e, sizeof e);
                NXLOG_W("movie", "av_read_frame: %s (treated as the end of the movie)", e);
            }
            eof_ = true;
        }
        for (Track& t : tr_) {
            if (!t.selected || !t.dec || t.eof_sent) continue;
            t.eof_sent = true;
            decode(t, nullptr);   // flush: the frames the decoder still holds
            t.drained = true;
        }
        return true;
    }

    void decode(Track& t, AVPacket* pkt) {
        int r = avcodec_send_packet(t.dec, pkt);
        if (r < 0 && r != AVERROR_EOF && r != AVERROR(EAGAIN)) {
            if (r != AVERROR_INVALIDDATA) {
                char e[128];
                av_strerror(r, e, sizeof e);
                NXLOG_W("movie", "avcodec_send_packet: %s", e);
            }
            return;   // a damaged packet: skipped
        }
        Frame fr;
        for (;;) {
            r = avcodec_receive_frame(t.dec, fr.f);
            if (r < 0) break;   // EAGAIN (needs more input) or EOF (flushed); a damaged frame is just missing
            if (t.info.kind == 0) video_frame(t, fr.f); else audio_frame(t, fr.f);
            av_frame_unref(fr.f);
        }
    }

    void video_frame(Track& t, const AVFrame* f) {
        int64_t pts = f->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = f->pts;
        if (pts == AV_NOPTS_VALUE) return;
        const int64_t us = av_rescale_q(pts, t.st->time_base, kUs) - base_us_;
        if (us < skip_before_us_) return;                        // between the keyframe a seek landed on and the target
        const int w = t.info.w, h = t.info.h;
        if (w <= 0 || h <= 0 || f->width <= 0 || f->height <= 0) return;
        const bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P;
        t.sws = sws_getCachedContext(t.sws, f->width, f->height, static_cast<AVPixelFormat>(f->format), w, h, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!t.sws) return;
        // same matrix and range on both sides: the planes are only repacked (and scaled when the size changed), never recoloured
        const int* coef = sws_getCoefficients(SWS_CS_DEFAULT);
        sws_setColorspaceDetails(t.sws, coef, full ? 1 : 0, coef, full ? 1 : 0, 0, 1 << 16, 1 << 16);
        const size_t uvrows = static_cast<size_t>((h + 1) / 2);
        SourceSample smp;
        smp.us = us;
        smp.data.resize(static_cast<size_t>(w) * h + static_cast<size_t>(w) * uvrows);
        if (w % 32 == 0 && h % 2 == 0) {
            uint8_t* dst[4] = {smp.data.data(), smp.data.data() + static_cast<size_t>(w) * h, nullptr, nullptr};
            const int stride[4] = {w, w, 0, 0};
            if (sws_scale(t.sws, f->data, f->linesize, 0, f->height, dst, stride) <= 0) return;
        } else {   // an odd or unaligned size: convert into padded planes, then pack the rows
            const int sw = (w + 1) & ~1, sh = (h + 1) & ~1;
            std::vector<uint8_t> tmp(static_cast<size_t>(sw) * sh * 3 / 2 + 64);
            uint8_t* dst[4] = {tmp.data(), tmp.data() + static_cast<size_t>(sw) * sh, nullptr, nullptr};
            const int stride[4] = {sw, sw, 0, 0};
            if (sws_scale(t.sws, f->data, f->linesize, 0, f->height, dst, stride) <= 0) return;
            for (int y = 0; y < h; y++) std::memcpy(smp.data.data() + static_cast<size_t>(y) * w, tmp.data() + static_cast<size_t>(y) * sw, static_cast<size_t>(w));
            for (size_t y = 0; y < uvrows; y++) std::memcpy(smp.data.data() + static_cast<size_t>(w) * h + y * w, dst[1] + y * sw, static_cast<size_t>(w));
        }
        t.ready.push_back(std::move(smp));
    }

    void audio_frame(Track& t, const AVFrame* f) {
        const int rate = t.info.rate, ch = t.info.ch;
        if (rate <= 0 || ch <= 0 || f->nb_samples <= 0) return;
        if (!t.rs.ensure(f, rate, ch)) { NXLOG_W("movie", "cannot set up the audio conversion for sample format %d", f->format); return; }
        int64_t us;
        if (f->pts != AV_NOPTS_VALUE) us = av_rescale_q(f->pts, t.st->time_base, kUs) - base_us_;
        else if (t.next_audio_us != INT64_MIN) us = t.next_audio_us;
        else us = 0;
        // The time of a block is where the data before it ends: a container that counts in milliseconds (WebM) rounds the time of every Opus packet
        // by up to 0.5 ms, and the first frame of a stream is shortened by the decoder (the pre-skip). Only a real gap or jump takes the packet's time.
        if (t.next_audio_us != INT64_MIN && f->pts != AV_NOPTS_VALUE && std::llabs(us - t.next_audio_us) < 25000) us = t.next_audio_us;
        const int cap = swr_get_out_samples(t.rs.swr, f->nb_samples) + 16;
        SourceSample smp;
        smp.us = us;
        smp.data.resize(static_cast<size_t>(cap) * ch * 2);
        uint8_t* out[1] = {smp.data.data()};
        const int n = swr_convert(t.rs.swr, out, cap, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
        if (n <= 0) return;
        smp.data.resize(static_cast<size_t>(n) * ch * 2);
        t.next_audio_us = us + static_cast<int64_t>(n) * 1000000 / rate;
        if (t.next_audio_us <= skip_before_us_) return;          // wholly before the position a seek asked for
        t.ready.push_back(std::move(smp));
    }

    MemIo io_;
    AVIOContext* io_ctx_ = nullptr;
    AVFormatContext* fmt_ = nullptr;
    std::vector<Track> tr_;
    std::vector<SourceTrack> pub_;
    int64_t duration_us_ = 0, base_us_ = 0, skip_before_us_ = 0;
    bool eof_ = false;
    int threads_ = 1;
};

}

std::unique_ptr<MovieSource> make_ffmpeg_source() { return std::make_unique<FfmpegSource>(); }

}
