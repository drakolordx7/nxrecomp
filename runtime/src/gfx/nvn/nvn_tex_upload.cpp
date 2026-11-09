// SPDX-License-Identifier: GPL-3.0-or-later
// Texture upload hook (nxrt/tex_upload.h): the list of subscribers, the edited data kept in memory, the background rebuild after a change, and the
// small BC7 textures a subscriber replaces whole. prepare_texture (nvn_texdata.cpp) does the decode, the subscriber's edit and the encode; this
// file decides when.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

#include <nxrt/config.h>
#include <nxrt/log.h>
#include <nxrt/tex_upload.h>

#include "astc2bc7.h"
#include "nvn_gpu.h"
#include "nvn_settings.h"
#include "nvn_gpu_internal.h"
#include "nvn_texdata.h"

namespace nxrt::nvn::be {
namespace {

constexpr const char* kTag = "nvn.texedit";

constexpr int kSlotShift = 56;      // a subscriber's ids are kept apart by its handle in the top byte
constexpr int kMaxHandle = 255;

struct MemoKey {
    uint64_t id, gen;
    VkFormat fmt;
    bool operator<(const MemoKey& o) const { return std::tie(id, gen, fmt) < std::tie(o.id, o.gen, o.fmt); }
};
struct Memo {
    bool ready = false;   // false: a thread is making it
    std::vector<uint8_t> data;
};

struct State {
    std::mutex mu;
    std::condition_variable cv_memo;   // a memo entry became ready or was given up
    struct Sub {
        int handle;
        gfx::TexUploadHooks hooks;
    };
    std::vector<Sub> subs;             // in the order they subscribed
    int next_handle = 1;
    std::map<MemoKey, Memo> memo;
    std::set<uint32_t> edited;         // Texture::id of the textures that were given edited data
    gfx::TexUploadStats st;
    // background rebuild
    std::condition_variable cv_work;
    bool want_rebuild = false;
    bool worker_started = false;
};

State& S() {
    static State* s = new State();   // never destroyed: the worker runs until the process ends
    return *s;
}

std::atomic<bool> g_active{false};

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::vector<State::Sub> subs_copy() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.subs;
}

// The first subscriber that wants the texture: its id with its handle in the top byte, 0 when none. Under SEH (a page the game unmapped must not
// take the process down).
uint64_t match_subs(const std::vector<State::Sub>& subs, const gfx::TexUploadReq& r) {
    __try {
        for (const State::Sub& sub : subs) {
            const uint64_t id = sub.hooks.match(r, sub.hooks.user);
            if (id && id < (1ull << kSlotShift)) return id | (static_cast<uint64_t>(sub.handle) << kSlotShift);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

// The sum of the subscribers' generations: any subscriber's change is a new generation (each only grows).
uint64_t generation_of(const std::vector<State::Sub>& subs) {
    uint64_t g = 0;
    for (const State::Sub& sub : subs)
        if (sub.hooks.generation) g += sub.hooks.generation(sub.hooks.user);
    return g;
}

bool prepare_guarded(const Texture& t, const TexCaps& caps, PreparedTexture& out) {
    __try {
        return prepare_texture(t, caps, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool candidate(const Texture& t, void*) {
    return t.mem && !t.is_display && t.fmt && t.fmt->astc && t.layers == 1 && t.b.samples <= 1 && t.b.target == NVN_TEXTURE_TARGET_2D;
}

gfx::TexUploadReq req_of(const Texture& t, const uint8_t* src) {
    gfx::TexUploadReq r;
    r.astc = t.fmt && t.fmt->astc;
    r.width = static_cast<uint32_t>(t.b.width);
    r.height = static_cast<uint32_t>(t.b.height);
    r.levels = t.levels;
    r.block_w = t.fmt ? t.fmt->block_w : 0;
    r.block_h = t.fmt ? t.fmt->block_h : 0;
    r.srgb = t.fmt && t.fmt->srgb;
    r.tiled = src;
    r.tiled_size = static_cast<size_t>(t.storage_size);
    return r;
}

// One pass after texture_upload_changed: new data for every live texture that matches, each uploaded again once its data is ready, and the ones
// edited before that no longer match uploaded again as the game's (they ask prepare_texture again, which finds the new data kept here).
void rebuild_once() {
    const std::vector<State::Sub> subs = subs_copy();
    const bool have = !subs.empty();
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Ref<Texture>> all = texture_registry_collect(&candidate, nullptr);
    std::set<uint32_t> was_edited;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        was_edited = S().edited;
    }
    TexCaps caps;
    bool caps_ok = false;
    if (gpu::active()) {
        const gfx::DeviceFeatures& f = gpu::device().features();
        caps.bc = f.bc;
        caps.astc = f.astc_ldr;
        caps.bc7_transcode = true;
        caps_ok = true;
    }
    std::vector<Texture*> dirty;
    std::vector<uint32_t> not_edited;   // edited before, the game's own data from now on
    uint32_t made = 0;
    for (const Ref<Texture>& r : all) {
        Texture* t = r.get();
        if (t->destroyed.load()) continue;
        const uint64_t id = have ? match_subs(subs, req_of(*t, t->mem)) : 0;
        if (id && caps_ok && !caps.astc) {
            PreparedTexture tmp;
            if (prepare_guarded(*t, caps, tmp)) ++made;   // keeps the edited data in the memo
        }
        if (id || was_edited.count(t->id)) {
            // uploaded again as soon as its own data is ready: the first textures change while the others are still being made
            dirty.push_back(t);
            t->cpu_dirty.store(true);
            gpu::mark_texture_cpu_dirty(t);
        }
        if (!id && was_edited.count(t->id)) not_edited.push_back(t->id);
    }
    {
        std::lock_guard<std::mutex> lk(S().mu);
        State& s = S();
        for (uint32_t i : not_edited) s.edited.erase(i);
        s.st.rebuilds++;
        s.st.last_rebuild_ms = ms_since(t0);
        s.st.last_rebuild_textures = made;
    }
    NXLOG_I(kTag, "rebuild: %u textures edited, %zu uploaded again, %.1f ms (%zu ASTC textures looked at)", made, dirty.size(), ms_since(t0), all.size());
}

void worker_main() {
    log_set_thread_name("nvn-texedit");
    State& s = S();
    std::unique_lock<std::mutex> lk(s.mu);
    for (;;) {
        s.cv_work.wait(lk, [&] { return s.want_rebuild; });
        s.want_rebuild = false;
        lk.unlock();
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        rebuild_once();
        lk.lock();
    }
}

}  // namespace

// ---- prepare_texture side (nvn_texdata.h) ---------------------------------------------------------------------------------------
bool tex_edit_active() { return g_active.load(std::memory_order_acquire); }

TexEditKey tex_edit_match(const Texture& t, const uint8_t* src) {
    TexEditKey k;
    const std::vector<State::Sub> subs = subs_copy();
    if (subs.empty()) return k;
    k.id = match_subs(subs, req_of(t, src));
    if (k.id) k.gen = generation_of(subs);
    return k;
}

bool tex_edit_lookup(const TexEditKey& k, VkFormat f, std::vector<uint8_t>& data) {
    State& s = S();
    std::unique_lock<std::mutex> lk(s.mu);
    const MemoKey mk{k.id, k.gen, f};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        auto it = s.memo.find(mk);
        if (it == s.memo.end()) {
            s.memo[mk];   // this thread makes it: tex_edit_store or tex_edit_abandon follows
            return false;
        }
        if (it->second.ready) {
            data = it->second.data;
            s.st.reuses++;
            return true;
        }
        if (s.cv_memo.wait_until(lk, deadline) == std::cv_status::timeout) return false;   // make it here as well rather than wait longer
    }
}

void tex_edit_store(const TexEditKey& k, VkFormat f, const std::vector<uint8_t>& data, double ms) {
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        const uint64_t cur = s.subs.empty() ? k.gen : generation_of(s.subs);
        Memo& m = s.memo[MemoKey{k.id, k.gen, f}];
        m.ready = true;
        m.data = data;
        // data of older answers is not asked for again
        for (auto it = s.memo.begin(); it != s.memo.end();) {
            if (it->second.ready && it->first.gen != cur) it = s.memo.erase(it);
            else ++it;
        }
        s.st.edits++;
        s.st.last_edit_ms = ms;
    }
    s.cv_memo.notify_all();
}

void tex_edit_abandon(const TexEditKey& k, VkFormat f) {
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        auto it = s.memo.find(MemoKey{k.id, k.gen, f});
        if (it != s.memo.end() && !it->second.ready) s.memo.erase(it);
    }
    s.cv_memo.notify_all();
}

bool tex_edit_level(uint64_t id, uint32_t level, uint32_t w, uint32_t h, uint8_t* rgba) {
    const int handle = static_cast<int>(id >> kSlotShift);
    gfx::TexUploadHooks hk;
    {
        std::lock_guard<std::mutex> lk(S().mu);
        bool found = false;
        for (const State::Sub& sub : S().subs)
            if (sub.handle == handle) {
                hk = sub.hooks;
                found = true;
            }
        if (!found) return false;
    }
    return hk.edit && hk.edit(id & ((1ull << kSlotShift) - 1), level, w, h, rgba, hk.user);
}

// A single-level BC7 sRGB texture of at most 128 x 128 (button icons and the like): the first subscriber that wants it supplies the whole image, which is
// encoded to BC7 at the same size into `bc7` (the tight linear blocks of the w x h texture). false: the game's blocks stay.
bool tex_upload_bc7(const Texture& t, uint32_t w, uint32_t h, std::vector<uint8_t>& bc7) {
    if (!tex_edit_active() || !t.mem) return false;
    const std::vector<State::Sub> subs = subs_copy();
    gfx::TexUploadReq req = req_of(t, t.mem);
    req.width = w;
    req.height = h;
    const uint64_t id = match_subs(subs, req);
    if (!id) return false;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    if (!tex_edit_level(id, 0, w, h, rgba.data())) return false;
    a2b_options opt{};
    opt.quality = A2B_DEFAULT_QUALITY;
    opt.threads = 1;
    opt.allow_opaque_mode6 = 0;
    return a2b_encode_bc7(w, h, rgba.data(), &opt, bc7.data(), bc7.size(), nullptr) == A2B_OK;
}

void tex_edit_note(const Texture& t) {
    std::lock_guard<std::mutex> lk(S().mu);
    S().edited.insert(t.id);
}

int tex_edit_quality() {
    return static_cast<int>(nvn_settings().tex_edit_quality);
}

}  // namespace nxrt::nvn::be

namespace nxrt::gfx {

int texture_upload_subscribe(const TexUploadHooks& hooks) {
    using namespace nxrt::nvn::be;
    State& s = S();
    if (!hooks.match) return 0;
    int handle = 0;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        for (int tries = 0; tries < kMaxHandle && !handle; ++tries) {   // the next free handle, 1 .. 255
            const int h = s.next_handle;
            s.next_handle = h % kMaxHandle + 1;
            bool used = false;
            for (const State::Sub& sub : s.subs) used |= sub.handle == h;
            if (!used) handle = h;
        }
        if (!handle) return 0;
        s.subs.push_back(State::Sub{handle, hooks});
        s.memo.clear();
        g_active.store(true, std::memory_order_release);
    }
    texture_upload_changed();
    return handle;
}

void texture_upload_unsubscribe(int handle) {
    using namespace nxrt::nvn::be;
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        const size_t n = s.subs.size();
        s.subs.erase(std::remove_if(s.subs.begin(), s.subs.end(), [&](const State::Sub& sub) { return sub.handle == handle; }), s.subs.end());
        if (s.subs.size() == n) return;
        s.memo.clear();
        g_active.store(!s.subs.empty(), std::memory_order_release);
    }
    texture_upload_changed();   // the changed textures go back to the game's
}

void texture_upload_changed() {
    using namespace nxrt::nvn::be;
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.subs.empty() || !s.edited.empty()) {
            s.want_rebuild = true;
            if (!s.worker_started) {
                s.worker_started = true;
                std::thread(&worker_main).detach();
            }
        }
    }
    s.cv_work.notify_one();
    textures_mark_bc7_candidates_dirty();   // the small BC7 textures are uploaded again too
}

TexUploadStats texture_upload_stats() {
    using namespace nxrt::nvn::be;
    std::lock_guard<std::mutex> lk(S().mu);
    return S().st;
}

}  // namespace nxrt::gfx
