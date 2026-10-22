// SPDX-License-Identifier: GPL-3.0-or-later
// Memory pools as Vulkan memory. A pool's storage is the game's own memory (a view of the runtime's pagefile-backed heap); it is
// imported with VK_EXT_external_memory_host and wrapped in one VkBuffer, so a GPU address (= host address) resolves to
// (buffer, offset) with no copy, and anything the GPU writes (compute, copies, queries, events) is visible to the CPU through
// the same pages. Measured on the RTX 3080 and the UHD 770 with MapViewOfFile views: imports work with 4096-byte alignment,
// and GPU writes are visible through every view of the section.
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <new>
#include <unordered_set>

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be {

struct PoolGpu {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    uint8_t* base = nullptr;          // aligned start of the import
    VkDeviceSize size = 0;            // aligned size of the import
    bool failed = false;
};

MemoryPool::~MemoryPool() {
    if (gpu) gpu::destroy_pool_gpu(gpu);
    gpu = nullptr;
    if (PoolGpu* pf = prefetched.exchange(nullptr)) gpu::destroy_pool_gpu(pf);   // imported in the background and never used
}

namespace gpu {
namespace {

constexpr const char* kTag = "nvn.pool";

VkBuffer g_dummy = VK_NULL_HANDLE;
gfx::Buffer g_dummy_buf;
std::once_flag g_dummy_once;
std::mutex g_log_mu;
std::unordered_set<uint64_t> g_logged;
std::mutex g_live_mu;
std::unordered_set<PoolGpu*> g_live;

PoolGpu* import_pool(MemoryPool* p) {
    ft::Scope zs(ft::zone_id("pool.import"), 0, static_cast<uint32_t>(p->flags) << 16 | static_cast<uint32_t>(std::min<uint64_t>(p->size >> 20, 0xffff)));
    auto* g = new PoolGpu();
    gfx::Device& dev = device();
    if (!dev.features().external_memory_host || !p->base || !p->size) {
        g->failed = true;
        NXLOG_E(kTag, "pool at %p size 0x%llx cannot be imported (%s)", static_cast<void*>(p->base), static_cast<unsigned long long>(p->size),
                !dev.features().external_memory_host ? "VK_EXT_external_memory_host is missing" : "no storage");
        return g;
    }
    const uint64_t align = std::max<uint64_t>(dev.limits_ext().min_imported_host_alignment, 4096);
    const uint64_t start = reinterpret_cast<uint64_t>(p->base) & ~(align - 1);
    const uint64_t end = (reinterpret_cast<uint64_t>(p->base) + p->size + align - 1) & ~(align - 1);
    VkDeviceSize size = end - start;
    if (size > dev.limits_ext().max_buffer_size && dev.limits_ext().max_buffer_size) {
        NXLOG_W(kTag, "pool of 0x%llx bytes is larger than maxBufferSize 0x%llx: the tail is not reachable by the GPU",
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(dev.limits_ext().max_buffer_size));
        size = dev.limits_ext().max_buffer_size & ~(align - 1);
    }
    g->base = reinterpret_cast<uint8_t*>(start);
    g->size = size;

    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r;
    { NXFT("pool.hostprops"); r = vkGetMemoryHostPointerPropertiesEXT(dev.device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, g->base, &hp); }
    if (r != VK_SUCCESS || !hp.memoryTypeBits) {
        NXLOG_E(kTag, "vkGetMemoryHostPointerPropertiesEXT failed (%d) for %p", static_cast<int>(r), static_cast<void*>(g->base));
        g->failed = true;
        return g;
    }
    uint32_t ty = 0;
    while (!(hp.memoryTypeBits & (1u << ty))) ++ty;
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = g->base;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
    mai.allocationSize = size;
    mai.memoryTypeIndex = ty;
    { NXFT("pool.alloc"); r = vkAllocateMemory(dev.device(), &mai, nullptr, &g->mem); }
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "importing pool %p (0x%llx bytes) failed (%d)", static_cast<void*>(g->base), static_cast<unsigned long long>(size), static_cast<int>(r));
        g->failed = true;
        return g;
    }
    VkExternalMemoryBufferCreateInfo ebi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ebi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(dev.device(), &bci, nullptr, &g->buffer);
    if (r == VK_SUCCESS) r = vkBindBufferMemory(dev.device(), g->buffer, g->mem, 0);
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "buffer over pool %p failed (%d)", static_cast<void*>(g->base), static_cast<int>(r));
        g->failed = true;
        return g;
    }
    counters().pools_imported.fetch_add(1);
    { std::lock_guard<std::mutex> lk(g_live_mu); g_live.insert(g); }
    NXLOG_D(kTag, "pool %p imported: 0x%llx bytes, memory type %u", static_cast<void*>(g->base), static_cast<unsigned long long>(size), ty);
    return g;
}

// ---- background import --------------------------------------------------------------------------------------------------
// One worker thread imports pools as they are created. The pool is kept alive by a reference while it is queued or running; the render
// thread, when it needs a pool that is still in flight, waits for it instead of importing a second time.
std::mutex g_pf_mu;
std::condition_variable g_pf_cv;
std::deque<Ref<MemoryPool>> g_pf_queue;
std::thread g_pf_thread;
bool g_pf_stop = false;

void prefetch_loop() {
    log_set_thread_name("nvn-pool-import");
    for (;;) {
        Ref<MemoryPool> p;
        {
            std::unique_lock<std::mutex> lk(g_pf_mu);
            g_pf_cv.wait(lk, [] { return g_pf_stop || !g_pf_queue.empty(); });
            if (g_pf_stop) return;
            p = std::move(g_pf_queue.front());
            g_pf_queue.pop_front();
        }
        int expect = 1;
        if (!p->prefetch.compare_exchange_strong(expect, 3)) continue;   // the render thread needed it first and imports it itself
        PoolGpu* g = nullptr;
        if (!p->dead.load()) g = import_pool(p.get());
        {
            std::lock_guard<std::mutex> lk(g_pf_mu);
            p->prefetched.store(g);
            p->prefetch.store(2);
        }
        g_pf_cv.notify_all();
    }
}

void make_dummy() {
    gfx::Device& dev = device();
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (dev.create_buffer(65536, usage, gfx::MemoryUse::Upload, &g_dummy_buf, "nvn dummy buffer") == VK_SUCCESS) {
        std::memset(g_dummy_buf.mapped, 0, 65536);
        g_dummy = g_dummy_buf.buffer;
    }
}

}  // namespace

void prefetch_pool(MemoryPool* p) {
    static const bool enabled = config_get_bool("nvn", "pool_prefetch", true);   // [nvn] pool_prefetch = false: import on first use only
    if (!enabled || !p || p->is_virtual || !p->base || !p->size || !active()) return;
    std::lock_guard<std::mutex> lk(g_pf_mu);
    if (g_pf_stop) return;
    int expect = 0;
    if (!p->prefetch.compare_exchange_strong(expect, 1)) return;
    if (!g_pf_thread.joinable()) g_pf_thread = std::thread(prefetch_loop);
    g_pf_queue.push_back(Ref<MemoryPool>::share(p));
    g_pf_cv.notify_all();
}

VkBuffer dummy_buffer() {
    std::call_once(g_dummy_once, make_dummy);
    return g_dummy;
}

BufRef resolve_buffer(uint64_t gpu_address, uint64_t size) {
    BufRef out;
    const PoolHit hit = pool_lookup(gpu_address);
    PoolGpu* pg = nullptr;
    if (hit.pool) {
        if (!hit.pool->gpu) {
            MemoryPool* mp = hit.pool;
            int expect = 1;
            if (mp->prefetch.load() == 1 && mp->prefetch.compare_exchange_strong(expect, 4)) {
                // still queued: import it here and now rather than behind the pools queued before it
            } else if (mp->prefetch.load() == 3) {
                NXFT("pool.wait");
                std::unique_lock<std::mutex> lk(g_pf_mu);
                g_pf_cv.wait(lk, [mp] { return mp->prefetch.load() == 2 || g_pf_stop; });
            }
            if (mp->prefetch.load() == 2) mp->gpu = mp->prefetched.exchange(nullptr);
            if (!mp->gpu) mp->gpu = import_pool(mp);
            ft::mark("pool.used", static_cast<uint32_t>(mp->flags) << 16 | static_cast<uint32_t>(std::min<uint64_t>(mp->size >> 20, 0xffff)));
        }
        pg = hit.pool->gpu;
    }
    if (!pg || pg->failed || !pg->buffer) {
        counters().unresolved_addresses.fetch_add(1);
        std::lock_guard<std::mutex> lk(g_log_mu);
        if (g_logged.insert(gpu_address >> 12).second)
            NXLOG_W(kTag, "GPU address 0x%llx (+0x%llx) is not in an importable memory pool: the dummy buffer is bound", static_cast<unsigned long long>(gpu_address),
                    static_cast<unsigned long long>(size));
        out.buffer = dummy_buffer();
        out.avail = 65536;
        return out;
    }
    // the import starts at the aligned base, which may be below the pool's own base
    const uint64_t off = gpu_address - reinterpret_cast<uint64_t>(pg->base);
    if (off >= pg->size) {
        counters().unresolved_addresses.fetch_add(1);
        out.buffer = dummy_buffer();
        out.avail = 65536;
        return out;
    }
    out.buffer = pg->buffer;
    out.offset = off;
    out.avail = pg->size - off;
    out.ok = size <= out.avail;
    return out;
}

void pool_shutdown() {
    {   // stop the importer first: a pool it is working on finishes, queued ones are dropped
        std::unique_lock<std::mutex> lk(g_pf_mu);
        g_pf_stop = true;
        g_pf_queue.clear();
    }
    g_pf_cv.notify_all();
    if (g_pf_thread.joinable()) g_pf_thread.join();
    {
        std::lock_guard<std::mutex> lk(g_pf_mu);
        g_pf_stop = false;           // a later back end (tests bring it up and down) starts clean
    }
    std::vector<PoolGpu*> left;
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        left.assign(g_live.begin(), g_live.end());
    }
    for (PoolGpu* p : left) pool_gpu_destroy(p);
    if (g_dummy_buf.buffer) device().destroy_buffer(g_dummy_buf);
    g_dummy = VK_NULL_HANDLE;
    g_dummy_buf = gfx::Buffer();
    new (&g_dummy_once) std::once_flag();
}

uint64_t pool_imports() { return counters().pools_imported.load(); }

void pool_gpu_destroy(PoolGpu* p) {
    if (!p) return;
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        if (!g_live.erase(p) && !p->failed) return;   // already destroyed by pool_shutdown
    }
    gfx::Device& dev = device();
    if (p->buffer) vkDestroyBuffer(dev.device(), p->buffer, nullptr);
    if (p->mem) vkFreeMemory(dev.device(), p->mem, nullptr);
    delete p;
}

}  // namespace gpu
}  // namespace nxrt::nvn::be
