// SPDX-License-Identifier: GPL-3.0-or-later
// Registry of initialized textures by storage range (see nvn_core.h).
#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nvn_format.h"
#include "nvn_gpu.h"

namespace nxrt::nvn::be {
namespace {
std::mutex g_mu;
std::multimap<uint64_t, Texture*> g_by_start;   // storage start -> texture
}  // namespace

void texture_registry_add(Texture* t) {
    if (!t || !t->mem) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_by_start.emplace(reinterpret_cast<uint64_t>(t->mem), t);
}

void texture_registry_remove(Texture* t) {
    if (!t || !t->mem) return;
    std::lock_guard<std::mutex> lk(g_mu);
    auto r = g_by_start.equal_range(reinterpret_cast<uint64_t>(t->mem));
    for (auto it = r.first; it != r.second; ++it)
        if (it->second == t) { g_by_start.erase(it); break; }
}

void tev(const Texture* t, const char* what, uint64_t a, uint64_t b) {
    static const bool on = nxrt::config_get_int("nvn", "trace_tex_events", 0) != 0;
    if (!on || !t || !t->fmt || t->b.width != 128 || t->layers != 6 || t->levels != 8) return;
    NXLOG_I("nvn.tev", "t%u %s mem %p size %llu: %llx %llx", t->id, what, static_cast<const void*>(t->mem), static_cast<unsigned long long>(t->storage_size),
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
}

void textures_cpu_wrote(const void* lo, uint64_t size) {
    const uint64_t a = reinterpret_cast<uint64_t>(lo), b = a + size;
    std::vector<Ref<Texture>> hit;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        // textures start at or before b; a texture can start before a and reach into the range, so scan back a bounded amount
        auto it = g_by_start.upper_bound(b - 1);
        while (it != g_by_start.begin()) {
            --it;
            Texture* t = it->second;
            const uint64_t ts = it->first, te = ts + std::max<uint64_t>(t->storage_size, 1);
            if (te > a && ts < b) { t->ref(); hit.emplace_back(t); }
            else if (ts + (64ull << 20) < a) break;   // textures are far smaller than 64 MiB
        }
    }
    for (auto& t : hit) {
        tev(t.get(), "cpu_wrote", reinterpret_cast<uint64_t>(lo), size);
        t->cpu_dirty.store(true);
        gpu::mark_texture_cpu_dirty(t.get());
    }
}

size_t texture_registry_size() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_by_start.size();
}

}  // namespace nxrt::nvn::be
