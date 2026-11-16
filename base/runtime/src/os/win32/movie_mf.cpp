// SPDX-License-Identifier: GPL-3.0-or-later
// The Media Foundation backend of the movie:: library (include/nxrt/movie_source.h): the source reader demuxes and decodes H.264 and AAC in
// an MP4. A fallback behind `[media] backend = mf`; FFmpeg (src/media/movie_ffmpeg.cpp) is the default on both platforms and plays what Media
// Foundation cannot (VP9, Opus and Vorbis in WebM need extensions from the Store). Remove this file once FFmpeg has been seen working in every game.
//
// Media Foundation hands out media timestamps: the edit list of the MP4 (leading empty edits, the start of the first real edit) is read from the
// boxes here and reported as SourceTrack::offset_us, so the first frame lands at 0 and the AAC priming falls before 0, as ffmpeg does.
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/movie_source.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace nxrt::media {
namespace {

thread_local bool t_com = false;
std::once_flag g_mf_once;
HRESULT g_mf_hr = E_FAIL;

// COM on the calling thread (any thread may use a source, one after the other) and MFStartup once.
bool mf_ready() {
    if (!t_com) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) t_com = true;
    }
    std::call_once(g_mf_once, [] {
        g_mf_hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        NXLOG_I("movie", "MFStartup 0x%08lx", static_cast<unsigned long>(g_mf_hr));
    });
    return SUCCEEDED(g_mf_hr);
}

// ---- the MP4 box walk: edit lists ----------------------------------------------------------------------------------------------
struct Mp4Track {
    uint32_t handler = 0;           // 'vide' / 'soun'
    uint32_t timescale = 0;
    int64_t delay_us = 0;           // leading empty edits: the track starts this much later
    int64_t media_time = 0;         // start of the first real edit, in the track's time scale
    bool has_edit = false;
    int64_t offset_us() const { return delay_us - (timescale ? media_time * 1000000 / static_cast<int64_t>(timescale) : 0); }   // presentation time of media time 0
};
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

struct Mp4Walker {
    const uint8_t* d;
    size_t n;
    uint32_t movie_ts = 1000;
    std::vector<Mp4Track> tracks;

    void walk(size_t off, size_t end, int depth, int trak) {
        while (off + 8 <= end) {
            uint64_t sz = be32(d + off);
            const uint32_t type = be32(d + off + 4);
            size_t hdr = 8;
            if (sz == 1 && off + 16 <= end) { sz = be64(d + off + 8); hdr = 16; }
            else if (sz == 0) sz = end - off;
            if (sz < hdr || off + sz > end) return;
            const uint8_t* body = d + off + hdr;
            const size_t blen = static_cast<size_t>(sz - hdr);
            auto is = [&](const char* s) { return type == be32(reinterpret_cast<const uint8_t*>(s)); };
            if (is("moov") || is("edts") || is("mdia")) {
                walk(off + hdr, off + static_cast<size_t>(sz), depth + 1, trak);
            } else if (is("trak")) {
                tracks.emplace_back();
                walk(off + hdr, off + static_cast<size_t>(sz), depth + 1, static_cast<int>(tracks.size()) - 1);
            } else if (is("mvhd") && blen >= 20) {
                movie_ts = body[0] == 1 ? (blen >= 24 ? be32(body + 20) : 1000) : be32(body + 12);
            } else if (is("mdhd") && trak >= 0 && blen >= 20) {
                tracks[trak].timescale = body[0] == 1 ? (blen >= 24 ? be32(body + 20) : 0) : be32(body + 12);
            } else if (is("hdlr") && trak >= 0 && blen >= 12) {
                tracks[trak].handler = be32(body + 8);
            } else if (is("elst") && trak >= 0 && blen >= 8) {
                const uint32_t cnt = be32(body + 4);
                const bool v1 = body[0] == 1;
                const size_t esz = v1 ? 20 : 12;
                int64_t delay = 0, media = 0;
                bool got = false;
                for (uint32_t k = 0; k < cnt && 8 + (k + 1) * esz <= blen && !got; k++) {
                    const uint8_t* e = body + 8 + k * esz;
                    const int64_t seg = v1 ? static_cast<int64_t>(be64(e)) : be32(e);
                    const int64_t mt = v1 ? static_cast<int64_t>(be64(e + 8)) : static_cast<int32_t>(be32(e + 4));
                    if (mt == -1) delay += seg * 1000000 / std::max<uint32_t>(movie_ts, 1);   // an empty edit: the track starts later
                    else { media = mt; got = true; }
                }
                Mp4Track& t = tracks[trak];
                t.has_edit = true;
                t.delay_us = delay;
                t.media_time = media;
            }
            off += static_cast<size_t>(sz);
        }
    }
};

int track_kind_of_native(IMFMediaType* t, GUID* sub) {
    GUID major{};
    t->GetGUID(MF_MT_MAJOR_TYPE, &major);
    t->GetGUID(MF_MT_SUBTYPE, sub);
    if (major == MFMediaType_Video) return 0;
    if (major == MFMediaType_Audio) return 1;
    return 2;
}
bool is_fourcc_guid(const GUID& g) {
    static const unsigned char tail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    return g.Data2 == 0 && g.Data3 == 0x10 && std::memcmp(g.Data4, tail, 8) == 0;
}
constexpr uint32_t fourcc_of(char a, char b, char c, char d) { return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24); }

class MfSource final : public MovieSource {
public:
    ~MfSource() override {
        mf_ready();   // COM on this thread (the extractor's closing thread): the reader's objects are released here
        rd_.Reset();
        bs_.Reset();
    }

    bool open(const uint8_t* data, size_t size, int container) override {
        if (!mf_ready()) return false;
        ComPtr<IStream> st;
        st.Attach(SHCreateMemStream(data, static_cast<UINT>(size)));
        if (!st) return false;
        HRESULT hr = MFCreateMFByteStreamOnStream(st.Get(), &bs_);
        if (FAILED(hr)) { NXLOG_W("movie", "MFCreateMFByteStreamOnStream 0x%08lx", static_cast<unsigned long>(hr)); return false; }
        ComPtr<IMFAttributes> ba;
        if (SUCCEEDED(bs_.As(&ba))) {
            ba->SetString(MF_BYTESTREAM_CONTENT_TYPE, container == 2 ? L"video/x-matroska" : container == 3 ? L"video/webm" : L"video/mp4");
            ba->SetString(MF_BYTESTREAM_ORIGIN_NAME, container == 2 ? L"movie.mkv" : container == 3 ? L"movie.webm" : L"movie.mp4");
        }
        hr = MFCreateSourceReaderFromByteStream(bs_.Get(), nullptr, &rd_);
        if (FAILED(hr)) { NXLOG_W("movie", "MFCreateSourceReaderFromByteStream 0x%08lx", static_cast<unsigned long>(hr)); return false; }
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(rd_->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)) && var.vt == VT_UI8)
            duration_us_ = static_cast<int64_t>(var.uhVal.QuadPart / 10);
        PropVariantClear(&var);
        rd_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);

        Mp4Walker mw{data, size};
        if (container == 1) mw.walk(0, size, 0, -1);

        std::vector<Entry> mf_tracks;
        for (DWORD i = 0;; i++) {
            ComPtr<IMFMediaType> nt;
            if (FAILED(rd_->GetNativeMediaType(i, 0, &nt))) break;
            GUID sub{};
            Entry en;
            SourceTrack& t = en.t;
            t.kind = track_kind_of_native(nt.Get(), &sub);
            en.mf_index = static_cast<int>(i);
            const uint32_t fcc = is_fourcc_guid(sub) ? sub.Data1 : 0;
            if (t.kind == 0) {
                UINT64 fs = 0, fr = 0;
                nt->GetUINT64(MF_MT_FRAME_SIZE, &fs);
                nt->GetUINT64(MF_MT_FRAME_RATE, &fr);
                t.w = static_cast<int>(fs >> 32);
                t.h = static_cast<int>(static_cast<uint32_t>(fs));
                t.fps = static_cast<uint32_t>(fr) ? static_cast<double>(fr >> 32) / static_cast<double>(static_cast<uint32_t>(fr)) : 0.0;
                UINT32 v = 0;
                nt->GetUINT32(MF_MT_AVG_BITRATE, &v);
                t.bitrate = static_cast<int>(v);
                // the game's colour conversion: 0 BT.601 limited, 1 BT.601 full, 2 BT.709 limited, 3 BT.709 full
                UINT32 mat = 0, range = 0;
                nt->GetUINT32(MF_MT_YUV_MATRIX, &mat);
                nt->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range);
                const bool bt601 = mat == MFVideoTransferMatrix_BT601;
                const bool full = range == MFNominalRange_0_255;
                t.colorspace = bt601 ? (full ? 1 : 0) : (full ? 3 : 2);
                t.mime = fcc == fourcc_of('V', 'P', '9', '0') ? "video/x-vnd.on2.vp9" : fcc == fourcc_of('V', 'P', '8', '0') ? "video/x-vnd.on2.vp8"
                         : (fcc == fourcc_of('H', '2', '6', '4') || fcc == fourcc_of('A', 'V', 'C', '1')) ? "video/avc" : fcc == fourcc_of('H', 'E', 'V', 'C') ? "video/hevc" : "video/unknown";
            } else if (t.kind == 1) {
                UINT32 rate = 0, ch = 0, br = 0;
                nt->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                nt->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
                nt->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &br);
                t.rate = static_cast<int>(rate);
                t.ch = static_cast<int>(ch);
                t.bitrate = static_cast<int>(br * 8);
                static const GUID kVorbis = {0x8D2FD10B, 0x5841, 0x4a6b, {0x89, 0x05, 0x58, 0x8F, 0xEC, 0x1A, 0xDE, 0xD9}};
                t.mime = sub == kVorbis ? "audio/vorbis" : fcc == 0x704F ? "audio/opus" : fcc == 0x1610 ? "audio/mp4a-latm" : fcc == 0x55 ? "audio/mpeg" : "audio/unknown";
            } else {
                t.mime = "application/octet-stream";
            }
            mf_tracks.push_back(std::move(en));
        }
        // file order: the i-th video (audio) trak of the file is the i-th video (audio) stream of the reader; the edit list of the trak
        // gives the track its time offset
        std::vector<Entry> ordered;
        {
            size_t used_v = 0, used_a = 0;
            const uint32_t vide = be32(reinterpret_cast<const uint8_t*>("vide")), soun = be32(reinterpret_cast<const uint8_t*>("soun"));
            for (const Mp4Track& mt : mw.tracks) {
                const int want = mt.handler == vide ? 0 : mt.handler == soun ? 1 : -1;
                if (want < 0) continue;
                size_t& used = want == 0 ? used_v : used_a;
                size_t seen = 0;
                for (Entry& en : mf_tracks) {
                    if (en.t.kind != want) continue;
                    if (seen++ != used) continue;
                    if (mt.has_edit) en.t.offset_us = mt.offset_us();
                    ordered.push_back(en);
                    break;
                }
                used++;
            }
            for (const Entry& en : mf_tracks) if (en.t.kind == 2) ordered.push_back(en);
        }
        tr_ = ordered.size() == mf_tracks.size() ? ordered : mf_tracks;
        for (const Entry& en : tr_) pub_.push_back(en.t);
        return true;
    }

    int64_t duration_us() const override { return duration_us_; }
    const std::vector<SourceTrack>& tracks() const override { return pub_; }

    SelectResult select(int i, bool on, SourceTrack* updated) override {
        mf_ready();
        if (!rd_ || i < 0 || i >= static_cast<int>(tr_.size())) return SelectResult::Failed;
        Entry& en = tr_[static_cast<size_t>(i)];
        SourceTrack& t = en.t;
        HRESULT hr = rd_->SetStreamSelection(static_cast<DWORD>(en.mf_index), on ? TRUE : FALSE);
        if (on && SUCCEEDED(hr) && t.kind != 2) {
            ComPtr<IMFMediaType> mt;
            MFCreateMediaType(&mt);
            if (t.kind == 0) {
                mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            } else {
                mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                mt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                mt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                mt->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(t.rate));
                mt->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, static_cast<UINT32>(t.ch));
            }
            hr = rd_->SetCurrentMediaType(static_cast<DWORD>(en.mf_index), nullptr, mt.Get());
            if (FAILED(hr) && t.kind == 1) {   // let the reader choose the PCM layout
                ComPtr<IMFMediaType> mt2;
                MFCreateMediaType(&mt2);
                mt2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                mt2->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                hr = rd_->SetCurrentMediaType(static_cast<DWORD>(en.mf_index), nullptr, mt2.Get());
            }
            if (SUCCEEDED(hr)) {
                ComPtr<IMFMediaType> cur;
                if (SUCCEEDED(rd_->GetCurrentMediaType(static_cast<DWORD>(en.mf_index), &cur))) {
                    if (t.kind == 0) {
                        UINT64 fs = 0;
                        cur->GetUINT64(MF_MT_FRAME_SIZE, &fs);
                        if (fs) { t.w = static_cast<int>(fs >> 32); t.h = static_cast<int>(static_cast<uint32_t>(fs)); }
                    } else {
                        UINT32 v = 0;
                        if (SUCCEEDED(cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &v))) t.rate = static_cast<int>(v);
                        if (SUCCEEDED(cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &v))) t.ch = static_cast<int>(v);
                    }
                }
            } else {
                NXLOG_W("movie", "track %d (%s): no Media Foundation decoder path (0x%08lx)", i, t.mime.c_str(), static_cast<unsigned long>(hr));
                rd_->SetStreamSelection(static_cast<DWORD>(en.mf_index), FALSE);
                return SelectResult::Unsupported;
            }
        }
        if (updated) *updated = t;
        return SUCCEEDED(hr) ? SelectResult::Ok : SelectResult::Failed;
    }

    bool seek(int64_t us) override {
        mf_ready();
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_I8;
        var.hVal.QuadPart = us * 10;
        return SUCCEEDED(rd_->SetCurrentPosition(GUID_NULL, var));
    }

    ReadStatus read(int track, SourceSample* out) override {
        mf_ready();
        const Entry& en = tr_[static_cast<size_t>(track)];
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        const HRESULT hr = rd_->ReadSample(static_cast<DWORD>(en.mf_index), 0, &idx, &flags, &ts, &s);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
            NXLOG_W("movie", "ReadSample(track %d) failed (0x%08lx, flags 0x%lx)", track, static_cast<unsigned long>(hr), flags);
            return ReadStatus::Error;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return ReadStatus::End;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) NXLOG_W("movie", "track %d: media type changed mid-stream", track);   // the game does not expect that
        if (!s) return ReadStatus::Again;
        out->us = static_cast<int64_t>(ts / 10);
        return convert(en.t, s.Get(), out) ? ReadStatus::Sample : ReadStatus::Error;
    }

private:
    struct Entry {
        SourceTrack t;
        int mf_index = 0;
    };

    // The decoded sample into the tightly packed layout of SourceSample.
    static bool convert(const SourceTrack& t, IMFSample* s, SourceSample* out) {
        ComPtr<IMFMediaBuffer> mb;
        if (FAILED(s->ConvertToContiguousBuffer(&mb)) || !mb) return false;
        if (t.kind != 0) {
            BYTE* data = nullptr;
            DWORD len = 0;
            if (FAILED(mb->Lock(&data, nullptr, &len)) || !data) return false;
            out->data.assign(data, data + len);
            mb->Unlock();
            return true;
        }
        ComPtr<IMF2DBuffer> b2;
        BYTE* scan = nullptr;
        LONG pitch = 0;
        DWORD len = 0;
        BYTE* data = nullptr;
        bool locked2d = false;
        if (SUCCEEDED(mb.As(&b2)) && SUCCEEDED(b2->Lock2D(&scan, &pitch)) && scan && pitch > 0) {
            locked2d = true;
            data = scan;
            mb->GetCurrentLength(&len);
        } else if (SUCCEEDED(mb->Lock(&data, nullptr, &len)) && data) {
            pitch = t.w;
        } else {
            return false;
        }
        // The decoder pads the picture to whole macroblocks: the chroma plane starts after the padded luma plane (1600x900 is stored as
        // 1600x912). The padded height is what the buffer length says: len = pitch * padded_height * 3 / 2.
        const int w = t.w, h = t.h;
        int padded = pitch > 0 ? static_cast<int>(static_cast<uint64_t>(len) * 2 / 3 / static_cast<uint64_t>(pitch)) : h;
        if (padded < h) padded = h;
        const size_t ysize = static_cast<size_t>(w) * h, uvrows = static_cast<size_t>((h + 1) / 2);
        out->data.resize(ysize + static_cast<size_t>(w) * uvrows);
        uint8_t* dst = out->data.data();
        for (int y = 0; y < h; y++) std::memcpy(dst + static_cast<size_t>(y) * w, data + static_cast<size_t>(y) * pitch, static_cast<size_t>(w));
        const BYTE* uv = data + static_cast<size_t>(pitch) * static_cast<size_t>(padded);
        for (size_t y = 0; y < uvrows; y++) std::memcpy(dst + ysize + y * static_cast<size_t>(w), uv + y * static_cast<size_t>(pitch), static_cast<size_t>(w));
        if (locked2d) b2->Unlock2D(); else mb->Unlock();
        return true;
    }

    ComPtr<IMFSourceReader> rd_;
    ComPtr<IMFByteStream> bs_;
    std::vector<Entry> tr_;
    std::vector<SourceTrack> pub_;
    int64_t duration_us_ = 0;
};

}

std::unique_ptr<MovieSource> make_mf_source() {
    if (!mf_ready()) return nullptr;
    return std::make_unique<MfSource>();
}

}
