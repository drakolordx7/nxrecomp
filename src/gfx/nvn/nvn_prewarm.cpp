// SPDX-License-Identifier: GPL-3.0-or-later
// Pipeline prewarm (S10). A graphics pipeline is created the first time a draw needs its combination of shaders and baked state,
// and on the real GPU that costs from a few to a few hundred milliseconds on the render thread. Two things keep it off the frame:
//
//  * Recording. Every pipeline created at a draw is appended to the key file ([nvn] pipeline_keys, default pipeline_keys.bin): the hashes of
//    its shaders (the cache's code hashes, stable between runs) and the rest of the key (formats, blend, topology ...) as plain bytes.
//  * Prewarming. When the game sets the shaders of a program (nvnProgramSetShaders, at load time, far from the first draw) every recorded key
//    whose shaders are exactly that program's is queued; worker threads build those pipelines with the on-disk VkPipelineCache. The draw
//    that later needs one takes the finished pipeline (or waits for the one in flight, or builds it itself when it is still queued).
//
// The VkPipelineCache is also written to disk here: a background thread saves it while new pipelines appear, because a window that is
// closed ends the process without running the shutdown path.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"
#include "nvn_pipeline.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "nvn.prewarm";
constexpr char kMagic[8] = {'N', 'X', 'P', 'K', 'E', 'Y', 'S', '1'};
constexpr size_t kStageBytes = sizeof(const ShaderInfo*) * 3;
constexpr size_t kRestBytes = sizeof(GraphicsKey) - kStageBytes;
static_assert(offsetof(GraphicsKey, stage) == 0, "the shader pointers come first in GraphicsKey");
static_assert(offsetof(GraphicsKey, color_format) == kStageBytes, "the plain key bytes follow the shader pointers");
constexpr size_t kMaxRecords = 400000;

struct Rec {
    uint64_t h[3];                          // code hashes of vertex, fragment, geometry (0 = stage absent)
    uint8_t rest[kRestBytes];               // the key after the shader pointers, byte for byte
};

struct Job {
    GraphicsKey key;
    const ProgLayout* layout = nullptr;
    int state = 0;                          // 0 queued, 1 building, 2 done, 3 taken over by a draw (the worker skips it); guarded by g_mu
    VkPipeline pipe = VK_NULL_HANDLE;
};

std::mutex g_mu;
std::condition_variable g_cv;
bool g_inited = false, g_enabled = true, g_stop = false;
std::string g_path;
std::vector<Rec> g_recs;
std::unordered_multimap<uint64_t, size_t> g_by_vs;           // vertex shader hash -> index in g_recs
std::unordered_set<uint64_t> g_digests;                      // recorded or queued keys
std::deque<std::shared_ptr<Job>> g_queue;
std::unordered_map<uint64_t, std::vector<std::shared_ptr<Job>>> g_jobs;   // by GraphicsKey::hash()
std::vector<std::thread> g_workers;
std::thread g_saver;
PrewarmStats g_stats;

uint64_t parse_hash(const std::string& hex) {
    uint64_t v = 0;
    for (char ch : hex) {
        uint64_t d;
        if (ch >= '0' && ch <= '9') d = static_cast<uint64_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<uint64_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = static_cast<uint64_t>(ch - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    return v;
}

uint64_t digest_of(const uint64_t h[3], const uint8_t* rest) {
    uint64_t d = 1469598103934665603ull;
    auto mix = [&d](uint8_t b) { d = (d ^ b) * 1099511628211ull; };
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 8; ++k) mix(static_cast<uint8_t>(h[i] >> (8 * k)));
    for (size_t i = 0; i < kRestBytes; ++i) mix(rest[i]);
    return d;
}

void hashes_of(const GraphicsKey& key, uint64_t h[3]) {
    for (int i = 0; i < 3; ++i) h[i] = key.stage[i] ? parse_hash(key.stage[i]->hash) : 0;
}

void load_file() {
    std::ifstream f(g_path, std::ios::binary);
    if (!f) return;
    char magic[8];
    uint32_t size = 0, pad = 0;
    f.read(magic, 8);
    f.read(reinterpret_cast<char*>(&size), 4);
    f.read(reinterpret_cast<char*>(&pad), 4);
    if (!f || std::memcmp(magic, kMagic, 8) != 0 || size != kRestBytes) {
        NXLOG_W(kTag, "%s is not a key file of this build (the record layout changed): ignored, a new one is started", g_path.c_str());
        f.close();
        std::error_code ec;
        std::filesystem::remove(g_path, ec);                 // so the next record writes a fresh header
        return;
    }
    Rec r;
    while (g_recs.size() < kMaxRecords && f.read(reinterpret_cast<char*>(&r), sizeof r)) {
        if (!g_digests.insert(digest_of(r.h, r.rest)).second) continue;
        g_by_vs.emplace(r.h[0], g_recs.size());
        g_recs.push_back(r);
    }
    g_stats.loaded = g_recs.size();
}

void append_record(const Rec& r) {
    bool fresh = false;
    {
        std::ifstream t(g_path, std::ios::binary | std::ios::ate);
        fresh = !t || t.tellg() < 16;
    }
    std::ofstream f(g_path, std::ios::binary | std::ios::app);
    if (!f) return;
    if (fresh) {
        const uint32_t size = static_cast<uint32_t>(kRestBytes), pad = 0;
        f.write(kMagic, 8);
        f.write(reinterpret_cast<const char*>(&size), 4);
        f.write(reinterpret_cast<const char*>(&pad), 4);
    }
    f.write(reinterpret_cast<const char*>(&r), sizeof r);
}

void worker_main() {
    log_set_thread_name("nvn-prewarm");
    set_compile_thread_priority();
    for (;;) {
        std::shared_ptr<Job> j;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_stop || !g_queue.empty(); });
            if (g_stop) return;
            j = std::move(g_queue.front());
            g_queue.pop_front();
            if (j->state != 0) continue;       // a draw took it over
            j->state = 1;
        }
        VkPipeline p;
        {
            NXFT("prewarm.build");
            p = build_graphics_pipeline(j->key, *j->layout);
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            j->pipe = p;
            j->state = 2;
            ++g_stats.built;
        }
        g_cv.notify_all();
    }
}

void saver_main() {
    log_set_thread_name("nvn-cache-save");
    std::unique_lock<std::mutex> lk(g_mu);
    uint64_t logged = 0;
    while (!g_stop) {
        g_cv.wait_for(lk, std::chrono::seconds(2), [] { return g_stop; });
        if (g_stop) break;
        const uint64_t sig = g_stats.recorded + g_stats.queued * 3 + g_stats.built * 5 + g_stats.taken * 7 + g_stats.waited * 11 + g_stats.claimed * 13;
        if (sig != logged) {                       // the process may end without a shutdown: say what the prewarm did while it runs
            logged = sig;
            NXLOG_I(kTag, "prewarm so far: %llu recorded, %llu loaded, %llu queued, %llu built ahead, %llu taken by draws (%llu waited %.1f ms), %llu built by the draw itself",
                    static_cast<unsigned long long>(g_stats.recorded), static_cast<unsigned long long>(g_stats.loaded), static_cast<unsigned long long>(g_stats.queued),
                    static_cast<unsigned long long>(g_stats.built), static_cast<unsigned long long>(g_stats.taken), static_cast<unsigned long long>(g_stats.waited),
                    g_stats.wait_ms, static_cast<unsigned long long>(g_stats.claimed));
        }
        gpl_progress_log();
        if (pipeline_cache_dirty()) {
            lk.unlock();
            pipeline_cache_save();
            lk.lock();
        }
    }
}

}  // namespace

void prewarm_init() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_inited) return;
    g_inited = true;
    g_stop = false;
    g_enabled = config_get_bool("nvn", "prewarm", true);
    g_path = config_get_str("nvn", "pipeline_keys", "pipeline_keys.bin");
    if (!g_enabled || g_path.empty()) {
        g_enabled = false;
        return;
    }
    load_file();
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int64_t want = config_get_int("nvn", "prewarm_threads", std::clamp<int64_t>(hw / 6, 1, 4));
    for (int64_t i = 0; i < want; ++i) g_workers.emplace_back(worker_main);
    g_saver = std::thread(saver_main);
    NXLOG_I(kTag, "prewarm: %zu recorded pipelines from %s, %zu worker threads", g_recs.size(), g_path.c_str(), g_workers.size());
}

void prewarm_record(const GraphicsKey& key) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_enabled) return;
    Rec r;
    std::memset(&r, 0, sizeof r);
    hashes_of(key, r.h);
    if (!r.h[0]) return;                                        // a stage the cache has no hash for cannot be found again
    std::memcpy(r.rest, reinterpret_cast<const uint8_t*>(&key) + kStageBytes, kRestBytes);
    if (g_recs.size() >= kMaxRecords || !g_digests.insert(digest_of(r.h, r.rest)).second) return;
    g_by_vs.emplace(r.h[0], g_recs.size());
    g_recs.push_back(r);
    ++g_stats.recorded;
    append_record(r);
}

size_t prewarm_program(const ShaderInfo* const stages[6]) {
    if (!stages || !stages[0]) return 0;
    prewarm_init();
    std::unique_lock<std::mutex> lk(g_mu);
    if (!g_enabled || g_stop) return 0;
    uint64_t h[3];
    for (int i = 0; i < 3; ++i) h[i] = stages[i] ? parse_hash(stages[i]->hash) : 0;
    std::vector<Rec> matches;
    auto range = g_by_vs.equal_range(h[0]);
    for (auto it = range.first; it != range.second; ++it)
        if (g_recs[it->second].h[1] == h[1] && g_recs[it->second].h[2] == h[2]) matches.push_back(g_recs[it->second]);
    if (matches.empty()) return 0;
    lk.unlock();                                                 // prog_layout takes its own lock and calls Vulkan
    const ProgLayout* layout = prog_layout(stages);
    lk.lock();
    if (!layout || g_stop) return 0;
    size_t queued = 0;
    for (const Rec& r : matches) {
        auto job = std::make_shared<Job>();
        std::memset(&job->key, 0, sizeof job->key);
        for (int i = 0; i < 3; ++i) job->key.stage[i] = stages[i];
        std::memcpy(reinterpret_cast<uint8_t*>(&job->key) + kStageBytes, r.rest, kRestBytes);
        const uint64_t kh = job->key.hash();
        bool dup = false;
        for (auto& e : g_jobs[kh])
            if (e->key == job->key) { dup = true; break; }
        if (dup) continue;
        job->layout = layout;
        g_jobs[kh].push_back(job);
        g_queue.push_back(job);
        ++queued;
    }
    if (queued) {
        g_stats.queued += queued;
        g_cv.notify_all();
    }
    return queued;
}

bool prewarm_take(const GraphicsKey& key, VkPipeline* out) {
    std::unique_lock<std::mutex> lk(g_mu);
    if (!g_enabled) return false;
    auto it = g_jobs.find(key.hash());
    if (it == g_jobs.end()) return false;
    auto& vec = it->second;
    for (size_t i = 0; i < vec.size(); ++i) {
        std::shared_ptr<Job> j = vec[i];
        if (!(j->key == key)) continue;
        if (j->state == 0) {                                     // still queued: build it here instead of behind the others
            j->state = 3;
            vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(i));
            ++g_stats.claimed;
            return false;
        }
        if (j->state == 1) {
            const auto t0 = std::chrono::steady_clock::now();
            NXFT("prewarm.wait");
            g_cv.wait(lk, [&] { return j->state == 2 || g_stop; });
            g_stats.wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++g_stats.waited;
        }
        if (j->state != 2) return false;
        *out = j->pipe;
        vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(i));
        ++g_stats.taken;
        return true;
    }
    return false;
}

void prewarm_shutdown() {
    std::vector<std::thread> workers;
    std::thread saver;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_inited) return;
        g_stop = true;
        workers.swap(g_workers);
        saver = std::move(g_saver);
        g_queue.clear();
    }
    g_cv.notify_all();
    for (auto& t : workers) t.join();
    if (saver.joinable()) saver.join();
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [h, vec] : g_jobs)
        for (auto& j : vec)
            if (j->state == 2 && j->pipe) vkDestroyPipeline(vkdev(), j->pipe, nullptr);     // built and never used
    g_jobs.clear();
    NXLOG_I(kTag, "prewarm: %llu recorded, %llu loaded, %llu queued, %llu built ahead, %llu taken (%llu waited %.1f ms), %llu taken over by a draw",
            static_cast<unsigned long long>(g_stats.recorded), static_cast<unsigned long long>(g_stats.loaded), static_cast<unsigned long long>(g_stats.queued),
            static_cast<unsigned long long>(g_stats.built), static_cast<unsigned long long>(g_stats.taken), static_cast<unsigned long long>(g_stats.waited),
            g_stats.wait_ms, static_cast<unsigned long long>(g_stats.claimed));
    g_inited = false;
    g_stats = PrewarmStats{};
    g_recs.clear();
    g_by_vs.clear();
    g_digests.clear();
}

PrewarmStats prewarm_stats() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_stats;
}

// The game set a program's shaders: pipelines recorded for it are built whole; a program with none gets its shader parts compiled ahead
// instead (nvn_gpl.cpp), so a draw links its pipeline.
void prewarm_program_shaders(const ShaderInfo* const stages[6]) {
    gpl_program(stages, prewarm_program(stages) != 0);
}

void flush_caches() { pipeline_cache_save(); }

}  // namespace nxrt::nvn::be::gpu
