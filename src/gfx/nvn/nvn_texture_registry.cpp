// SPDX-License-Identifier: GPL-3.0-or-later
// Registry of initialized textures by storage range (see nvn_core.h).
#include <algorithm>
#include <cstring>
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
std::vector<Texture*> g_virtual;                // sparse textures
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

void texture_registry_add_virtual(Texture* t) {
    if (!t || !t->vpool) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_virtual.push_back(t);
}

void texture_registry_remove_virtual(Texture* t) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_virtual.erase(std::remove(g_virtual.begin(), g_virtual.end(), t), g_virtual.end());
}

namespace {
void mark_dirty_list(std::vector<Ref<Texture>>& hit, uint64_t a, uint64_t b) {
    for (auto& t : hit) {
        tev(t.get(), "cpu_wrote", a, b - a);
        t->cpu_dirty.store(true);
        gpu::mark_texture_cpu_dirty(t.get());
    }
}
}  // namespace

void virtual_textures_remapped(const MemoryPool* vpool, uint64_t voff, uint64_t size) {
    std::vector<Ref<Texture>> hit;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (Texture* t : g_virtual)
            if (t->vpool.get() == vpool && t->voffset < voff + size && t->voffset + t->storage_size > voff) { t->ref(); hit.emplace_back(t); }
    }
    mark_dirty_list(hit, voff, voff + size);
}

bool texture_gather_virtual(const Texture& t, std::vector<uint8_t>& out) {
    MemoryPool* vp = t.vpool.get();
    if (!vp || !t.storage_size) return false;
    out.assign(static_cast<size_t>(t.storage_size), 0);
    const uint64_t t0 = t.voffset, t1 = t.voffset + t.storage_size;
    std::lock_guard<std::mutex> lk(vp->vmu);
    for (const MemoryPool::VirtualMap& m : vp->vmaps) {
        const uint64_t a = std::max(t0, m.voff), b = std::min(t1, m.voff + m.size);
        if (a >= b || !m.phys || !m.phys->base) continue;
        const int64_t src = m.poff + static_cast<int64_t>(a - m.voff);
        if (src < 0 || static_cast<uint64_t>(src) + (b - a) > m.phys->size) continue;
        std::memcpy(out.data() + (a - t0), m.phys->base + src, static_cast<size_t>(b - a));
    }
    return true;
}

void textures_cpu_wrote(const void* lo, uint64_t size) {
    const uint64_t a = reinterpret_cast<uint64_t>(lo), b = a + size;
    std::vector<Ref<Texture>> hit;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        // sparse textures whose mapped tiles lie in the written physical memory
        for (Texture* t : g_virtual) {
            MemoryPool* vp = t->vpool.get();
            if (!vp) continue;
            std::lock_guard<std::mutex> vl(vp->vmu);
            bool over = false;
            for (const MemoryPool::VirtualMap& m : vp->vmaps) {
                if (!m.phys || !m.phys->base) continue;
                const uint64_t pa = reinterpret_cast<uint64_t>(m.phys->base) + static_cast<uint64_t>(m.poff), pb = pa + m.size;
                if (pa >= b || pb <= a) continue;
                if (m.voff < t->voffset + t->storage_size && m.voff + m.size > t->voffset) { over = true; break; }
            }
            if (over) { t->ref(); hit.emplace_back(t); }
        }
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

void texture_registry_detach_gpu() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& kv : g_by_start) kv.second->gpu = nullptr;
    for (Texture* t : g_virtual) t->gpu = nullptr;
}

Ref<Texture> texture_registry_find_alias(const Texture* t) {
    if (!t || !t->mem || t->is_display) return Ref<Texture>();
    std::lock_guard<std::mutex> lk(g_mu);
    auto r = g_by_start.equal_range(reinterpret_cast<uint64_t>(t->mem));
    for (auto it = r.first; it != r.second; ++it) {
        Texture* o = it->second;
        if (o == t || o->destroyed.load() || !o->gpu) continue;
        if (o->storage_size != t->storage_size || o->levels != t->levels || o->layers != t->layers || o->fmt != t->fmt || o->block_linear != t->block_linear ||
            o->block_height_gobs != t->block_height_gobs || o->b.target != t->b.target || o->b.width != t->b.width || o->b.height != t->b.height ||
            o->b.depth != t->b.depth || o->b.samples != t->b.samples)
            continue;
        o->ref();
        return Ref<Texture>(o);
    }
    return Ref<Texture>();
}

void textures_mark_override_candidates_dirty() {
    std::vector<Ref<Texture>> hit;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& kv : g_by_start) {
            Texture* t = kv.second;
            if (t->destroyed.load() || !t->fmt || t->fmt->vk != VK_FORMAT_BC7_SRGB_BLOCK || t->levels != 1 || t->layers != 1) continue;
            if (t->b.width > 128 || t->b.height > 128 || t->b.samples > 1) continue;
            t->ref();
            hit.emplace_back(t);
        }
    }
    for (auto& t : hit) {
        t->cpu_dirty.store(true);
        gpu::mark_texture_cpu_dirty(t.get());
    }
}

size_t texture_registry_size() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_by_start.size();
}

}  // namespace nxrt::nvn::be
