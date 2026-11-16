// SPDX-License-Identifier: GPL-3.0-or-later
// The decoder behind the movie:: library (src/media/movie_hle.cpp): one MovieSource per movie that has been handed to an Extractor. The
// protocol layer speaks the game's side (objects, buffers, queues, loops) and asks the source for demuxed and decoded samples; the source
// knows nothing about the game. Backends: FFmpeg (src/media/movie_ffmpeg.cpp, both platforms, the default) and Windows Media Foundation
// (src/os/win32/movie_mf.cpp, a fallback: [media] backend = mf).
//
// Calls on one source are serialised by the protocol layer (never two at once) but may come from different threads, one after the other.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nxrt::media {

struct SourceTrack {
    int kind = 2;                  // 0 video, 1 audio, 2 anything else
    std::string mime;              // video/avc, video/x-vnd.on2.vp9, audio/mp4a-latm, audio/opus, audio/vorbis ...
    int w = 0, h = 0;              // video: picture size in pixels
    int rate = 0, ch = 0;          // audio: sample rate and channel count of the PCM the source delivers
    int bitrate = 0;               // bits per second, 0 unknown
    int colorspace = 2;            // video, what the game's conversion expects: 0 BT.601 limited, 1 BT.601 full, 2 BT.709 limited, 3 BT.709 full
    double fps = 0;
    // Presentation time of the source's time 0, in microseconds. The protocol adds it to the time of every sample and subtracts it from a seek
    // target. FFmpeg delivers presentation times already (0); Media Foundation delivers media times and the edit list is applied by the caller.
    int64_t offset_us = 0;
};

// One decoded sample. Video: NV12, tightly packed (w bytes per row, h rows of luma, then (h + 1) / 2 rows of interleaved chroma), at the
// size select() reported. Audio: interleaved signed 16-bit PCM, rate and ch as select() reported.
struct SourceSample {
    std::vector<uint8_t> data;
    int64_t us = 0;
};

enum class ReadStatus { Sample, Again, End, Error };   // Again: nothing to deliver yet (a flush, a stream tick): ask again
enum class SelectResult { Ok, Unsupported, Failed };

class MovieSource {
public:
    virtual ~MovieSource() = default;
    // `data` (the whole file) outlives the source. container: 1 mp4, 2 mkv, 3 webm; the source may also sniff the data.
    virtual bool open(const uint8_t* data, size_t size, int container) = 0;
    virtual int64_t duration_us() const = 0;
    virtual const std::vector<SourceTrack>& tracks() const = 0;   // after open(): fixed; select() hands out the refreshed entry
    // Turns a track on or off. On: the track's decoder is set up and `updated` gets its final w/h/rate/ch.
    virtual SelectResult select(int track, bool on, SourceTrack* updated) = 0;
    // Positions every selected track at `us` (source clock, the offset_us of the track already taken off); queued samples are dropped.
    virtual bool seek(int64_t us) = 0;
    // The next decoded sample of a selected track, in order; `out->us` is in the source clock (the protocol adds offset_us).
    virtual ReadStatus read(int track, SourceSample* out) = 0;
};

// The two backends. ffmpeg: nullptr when the libraries cannot be used. mf: nullptr on a system without Media Foundation (src/os/posix).
std::unique_ptr<MovieSource> make_ffmpeg_source();
std::unique_ptr<MovieSource> make_mf_source();

}
