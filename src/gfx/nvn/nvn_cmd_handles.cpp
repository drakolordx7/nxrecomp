// SPDX-License-Identifier: GPL-3.0-or-later
// NVNcommandHandle table and the command-memory lifetime rule. On the console a handle is valid until the game records over
// the command memory the list used (or frees the memory pool). The back end keeps the same rule with an estimate of the
// memory a list uses (nvn_cmd_lifecycle in api_cmdbuf.cpp): a list is dropped when a newer list occupies an overlapping
// range of command memory, when its pool is finalized, or when its command buffer is finalized. There is no count limit: the
// game calls some lists (character draw lists, the map) long after recording them.
// The rule is taken from SuperMarioWonderRecomp docs/nvn/DEBUG.md #26, DEBUG-2 #90, #91 (MIT); the code is new.
#include <atomic>
#include <map>
#include <mutex>
#include <unordered_map>

#include "nvn_cmd.h"

namespace nxrt::nvn::be {
namespace {

struct Entry {
    Ref<CmdList> list;
    uint64_t start = 0, end = 0;
};

std::mutex g_mu;
std::unordered_map<uint64_t, Entry> g_handles;
std::map<uint64_t, uint64_t> g_regions;      // start -> handle (regions never overlap among live lists)
std::atomic<uint64_t> g_next{0x1000};
std::atomic<uint64_t> g_missing{0};

void drop_locked(uint64_t handle) {
    auto it = g_handles.find(handle);
    if (it == g_handles.end()) return;
    if (it->second.end > it->second.start) {
        auto r = g_regions.find(it->second.start);
        if (r != g_regions.end() && r->second == handle) g_regions.erase(r);
    }
    g_handles.erase(it);   // the list lives on if a replay or another list still holds it
}

}  // namespace

uint64_t handle_register(Ref<CmdList> list) {
    const uint64_t h = g_next.fetch_add(8);
    list->handle = h;
    std::lock_guard<std::mutex> lk(g_mu);
    if (list->mem_end > list->mem_start) {
        // drop every older list whose region overlaps this one
        auto it = g_regions.lower_bound(list->mem_start);
        if (it != g_regions.begin()) {
            auto prev = std::prev(it);
            auto e = g_handles.find(prev->second);
            if (e != g_handles.end() && e->second.end > list->mem_start) it = prev;
        }
        std::vector<uint64_t> drop;
        for (; it != g_regions.end() && it->first < list->mem_end; ++it) drop.push_back(it->second);
        for (uint64_t d : drop) drop_locked(d);
        g_regions[list->mem_start] = h;
    }
    Entry e;
    e.start = list->mem_start;
    e.end = list->mem_end;
    e.list = std::move(list);
    g_handles.emplace(h, std::move(e));
    return h;
}

Ref<CmdList> handle_lookup(uint64_t handle) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_handles.find(handle);
    if (it == g_handles.end()) {
        g_missing.fetch_add(1);
        return {};
    }
    it->second.list->calls.fetch_add(1);
    return it->second.list;
}

void handle_release(uint64_t handle) {
    std::lock_guard<std::mutex> lk(g_mu);
    drop_locked(handle);
}

void handles_release_pool(const MemoryPool* pool) {
    if (!pool || !pool->base) return;
    const uint64_t lo = pool->gpu_base, hi = pool->gpu_base + pool->size;
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<uint64_t> drop;
    for (auto& [h, e] : g_handles)
        if (e.end > e.start && e.start >= lo && e.start < hi) drop.push_back(h);
    for (uint64_t h : drop) drop_locked(h);
}

const char* op_name(Op op) {
    static const char* const kNames[] = {
    "Nop",
    "BindProgram",
    "BindBuffers",
    "BindTextures",
    "BindImages",
    "BindVertexAttribs",
    "BindVertexStreams",
    "BindBlend",
    "BindColor",
    "BindChannelMask",
    "BindDepthStencil",
    "BindPolygon",
    "BindMultisample",
    "SetTexturePool",
    "SetSamplerPool",
    "SetViewports",
    "SetScissors",
    "SetDepthRanges",
    "SetDepthBounds",
    "SetStencil",
    "SetBlendColor",
    "SetPointSize",
    "SetLineWidth",
    "SetPolygonOffsetClamp",
    "SetAlphaRef",
    "SetSampleMask",
    "SetRasterizerDiscard",
    "SetDepthClamp",
    "SetPrimitiveRestart",
    "SetPatchSize",
    "SetTessLevels",
    "SetRenderEnable",
    "SetRenderTargets",
    "SetCoverageModulationTable",
    "SetConservativeRaster",
    "DrawArrays",
    "DrawElements",
    "DrawIndirect",
    "DrawTexture",
    "ClearColor",
    "ClearDepthStencil",
    "ClearTexture",
    "ClearBuffer",
    "CopyBufferToBuffer",
    "CopyBufferToTexture",
    "CopyTextureToBuffer",
    "CopyTextureToTexture",
    "Downsample",
    "UpdateUniformBuffer",
    "Dispatch",
    "DispatchIndirect",
    "ReportCounter",
    "ReportValue",
    "ResetCounter",
    "Barrier",
    "FenceSync",
    "WaitSync",
    "SignalEvent",
    "WaitEvent",
    "CallCommands",
    "DiscardColor",
    "DiscardDepthStencil",
    "ResolveDepth"};
    const size_t i = static_cast<size_t>(op);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

size_t handles_live() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_handles.size();
}

uint64_t handles_missing() { return g_missing.load(); }

}  // namespace nxrt::nvn::be
