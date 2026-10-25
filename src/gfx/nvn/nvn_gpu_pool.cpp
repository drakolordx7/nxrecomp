// SPDX-License-Identifier: GPL-3.0-or-later
// Memory pools as Vulkan memory. A pool's storage is the game's own memory (a view of the runtime's pagefile-backed heap); it is
// imported with VK_EXT_external_memory_host and wrapped in one VkBuffer, so a GPU address (= host address) resolves to
// (buffer, offset) with no copy, and anything the GPU writes (compute, copies, queries, events) is visible to the CPU through
// the same pages. Measured on the RTX 3080 and the UHD 770 with MapViewOfFile views: imports work with 4096-byte alignment,
// and GPU writes are visible through every view of the section.
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <new>
#include <unordered_set>

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/log.h"
#include "nxrt/gauge.h"
#include "nxrt/memory.h"
#include "nxrt/write_watch.h"
#include "vk_gauges.h"
#include "nvn_gpu_internal.h"
#include "nvn_mirror_cap.h"

namespace nxrt::nvn::be {

namespace gpu { struct Mirror; }

struct PoolGpu {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    uint8_t* base = nullptr;          // aligned start of the import
    VkDeviceSize size = 0;            // aligned size of the import
    bool failed = false;
    void* view = nullptr;             // private small view of the heap section the import goes through (nullptr: imported at base)
    gpu::Mirror* mirror = nullptr;  // device-local copy of the pages draws read (S38, [nvn] pool_mirror); render thread
    bool mirror_tried = false;
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
nxrt::GaugeCounter g_gauge_pools_live("nvn.pools_live");   // pools imported and not yet destroyed (g_live)

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

    // Import through a small private view of the same section pages when the pool lies in the heap's primary view (see mem::heap_private_view).
    uint8_t* import_ptr = g->base;
    static const bool use_view = config_get_bool("nvn", "pool_view", true);      // [nvn] pool_view = 0: import the pool's own address (slow)
    if (use_view && mem::heap_exists() && start >= mem::kHeapBase && end <= mem::kHeapBase + mem::heap_reserved()) {
        const uint64_t off = start - mem::kHeapBase;
        const uint64_t voff = off & ~(mem::kAllocGranularity - 1);
        const uint64_t delta = off - voff;
        const uint64_t vsize = (delta + size + mem::kAllocGranularity - 1) & ~(mem::kAllocGranularity - 1);
        if (void* v = mem::heap_private_view(voff, std::min<uint64_t>(vsize, mem::heap_reserved() - voff))) {
            g->view = v;
            import_ptr = static_cast<uint8_t*>(v) + delta;
        }
    }

    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r;
    { NXFT("pool.hostprops"); r = vkGetMemoryHostPointerPropertiesEXT(dev.device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, import_ptr, &hp); }
    if (r != VK_SUCCESS || !hp.memoryTypeBits) {
        NXLOG_E(kTag, "vkGetMemoryHostPointerPropertiesEXT failed (%d) for %p", static_cast<int>(r), static_cast<void*>(g->base));
        g->failed = true;
        return g;
    }
    uint32_t ty = 0;
    while (!(hp.memoryTypeBits & (1u << ty))) ++ty;
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = import_ptr;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
    mai.allocationSize = size;
    mai.memoryTypeIndex = ty;
    { NXFT("pool.alloc"); r = vkAllocateMemory(dev.device(), &mai, nullptr, &g->mem); }
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "importing pool %p (0x%llx bytes) failed (%d)", static_cast<void*>(g->base), static_cast<unsigned long long>(size), static_cast<int>(r));
        g->failed = true;
        return g;
    }
    vk_gauge_memory_allocs().add(1);
    vk_gauge_memory_bytes().add(static_cast<int64_t>(size));
    VkExternalMemoryBufferCreateInfo ebi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ebi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                (device().features().transform_feedback ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0u);
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(dev.device(), &bci, nullptr, &g->buffer);
    if (r == VK_SUCCESS) vk_gauge_buffers().add(1);
    if (r == VK_SUCCESS) r = vkBindBufferMemory(dev.device(), g->buffer, g->mem, 0);
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "buffer over pool %p failed (%d)", static_cast<void*>(g->base), static_cast<int>(r));
        g->failed = true;
        return g;
    }
    counters().pools_imported.fetch_add(1);
    { std::lock_guard<std::mutex> lk(g_live_mu); g_live.insert(g); }
    g_gauge_pools_live.add(1);
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
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                     VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    if (dev.create_buffer(65536, usage, gfx::MemoryUse::Upload, &g_dummy_buf, "nvn dummy buffer") == VK_SUCCESS) {
        std::memset(g_dummy_buf.mapped, 0, 65536);
        // What the console delivers for a vertex attribute the game did not set up: (0, 0, 0, 1) in the shader's own type.
        const float f1[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const uint32_t i1[4] = {0, 0, 0, 1};
        std::memcpy(static_cast<uint8_t*>(g_dummy_buf.mapped) + kDummyAttrFloatOffset, f1, 16);
        std::memcpy(static_cast<uint8_t*>(g_dummy_buf.mapped) + kDummyAttrIntOffset, i1, 16);
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

namespace {
BufRef resolve_in_pool(uint64_t gpu_address, uint64_t size, MemoryPool** pool_out, PoolGpu** gpu_out);
}

BufRef resolve_buffer(uint64_t gpu_address, uint64_t size) { return resolve_in_pool(gpu_address, size, nullptr, nullptr); }

namespace {
BufRef resolve_in_pool(uint64_t gpu_address, uint64_t size, MemoryPool** pool_out, PoolGpu** gpu_out) {
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
    if (pool_out) *pool_out = hit.pool;
    if (gpu_out) *gpu_out = pg;
    return out;
}
}  // namespace

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
    bool was_live = false;
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        was_live = g_live.erase(p) != 0;
        if (!was_live && !p->failed) return;   // already destroyed by pool_shutdown
    }
    if (was_live) g_gauge_pools_live.sub(1);
    if (p->mirror) mirror_destroy(p);
    gfx::Device& dev = device();
    if (p->buffer) { vkDestroyBuffer(dev.device(), p->buffer, nullptr); vk_gauge_buffers().sub(1); }
    if (p->mem) {
        vkFreeMemory(dev.device(), p->mem, nullptr);
        vk_gauge_memory_allocs().sub(1);
        vk_gauge_memory_bytes().sub(static_cast<int64_t>(p->size));   // p->size is the size the allocation was made with
    }
    if (p->view) mem::heap_private_view_unmap(p->view);
    delete p;
}


// ---- device-local copies of pool pages (S38) ---------------------------------------------------------------------
// The game's vertex, index and uniform data live in its memory pools, which are host memory: every draw read them over PCIe (S34). A pool
// that draws read from gets a device-local buffer of the same size (the mirror). A page is copied into it, in a transfer at the start of a
// submission, after some draw wanted it; from then on draws whose whole range is in valid pages read the mirror. A page stops being valid
// when the CPU writes it (the write tracker made it read-only when it was copied, nxrt/write_watch.h) or when the GPU writes it (copies,
// UpdateUniformBuffer, reports, storage buffers: the replay calls note_gpu_write). Until it is copied again, draws that touch it read the
// pool itself, as before. A page written again soon after every copy (a ring buffer the game refills each frame) is "hot": it is left in
// host memory for a while instead of faulting and being copied every frame.
//
// [nvn] pool_mirror = on (default) | off.
struct Mirror {
    gfx::Buffer buf;
    VkBuffer src = VK_NULL_HANDLE;      // the pool's own buffer (the copies read it)
    mem::ww::Region* ww = nullptr;      // the CPU's writes to the pool's pages
    uint64_t npages = 0;
    std::vector<uint64_t> want;         // bitmap: a draw wanted the page while it was not valid
    std::vector<uint32_t> copied_at;    // present count of the last copy (0: never)
    std::vector<uint64_t> used;         // bitmap: a draw read the page from the copy since it was copied
    std::vector<uint8_t> score;         // copies in a row that the CPU overwrote before any draw read them
    std::vector<uint8_t> backoff;       // how often the page has been hot in a row (the hold doubles each time)
    std::vector<uint32_t> hot_until;    // present count until which the page is not copied
    bool any_want = false;
    int pool_flags = 0;
    int64_t st_written = 0, st_copied = 0;   // pages the CPU wrote after a copy, pages copied (summary line)
};

namespace {

enum class MirrorMode { Off, On };
constexpr uint64_t kPageBytes = 4096;
constexpr uint64_t kMaxBindPages = (64ull << 20) / kPageBytes;      // a binding over more than 64 MiB stays in the pool
constexpr uint64_t kCopyBudget = 64ull << 20;                        // bytes copied per submission at most (an area load can want hundreds of MiB)
constexpr uint8_t kHotHits = 2;                                      // two copies in a row overwritten before any draw read them: the page is hot ...
constexpr uint32_t kHotHold = 600;                                   // ... and is not copied for 600 presents, twice that the next time, ...
constexpr uint8_t kHotMaxShift = 4;                                  // ... up to 16 times

std::vector<Mirror*> g_mirrors;      // render thread
uint64_t g_mirror_bytes = 0;
bool g_mirror_on = false;            // this submission reads mirrors (mode on and, with [nvn] gpuperf_ab, an "on" window)
std::vector<uint64_t> g_written_tmp;
std::vector<VkBufferCopy> g_copy_tmp;

std::atomic<int64_t> g_st_dev_binds{0}, g_st_host_binds{0}, g_st_copied{0}, g_st_hot{0}, g_st_live_bytes{0};
// statistics for the summary line (render thread): binds by use (to the copy / to the pool / not eligible), pages made stale by GPU writes by use
int64_t g_use_dev[3] = {}, g_use_host[3] = {}, g_use_skip[3] = {};
int64_t g_wr_pages[6] = {}, g_wr_calls[6] = {};

MirrorMode mirror_mode() {
    static const MirrorMode m = [] {
        const std::string v = config_get_str("nvn", "pool_mirror", "on");
        if (v == "on" || v == "1" || v == "true") return MirrorMode::On;
        return MirrorMode::Off;
    }();
    return m;
}

// The cap of the copies ([nvn] pool_mirror_mb, or from the card's video memory when it is not set; nvn_mirror_cap.h), logged once.
uint64_t mirror_budget() {
    uint64_t vram = 0;
    if (VkPhysicalDevice pd = device().physical()) {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
            if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max<uint64_t>(vram, mp.memoryHeaps[h].size);
    }
    const int64_t set_mb = config_has("nvn", "pool_mirror_mb") ? std::max<int64_t>(0, config_get_int("nvn", "pool_mirror_mb", 0)) : -1;
    const MirrorCap c = mirror_cap(vram, set_mb);
    NXLOG_I(kTag, "pool copies: cap %llu MiB (%s; largest device-local heap %llu MiB)%s", static_cast<unsigned long long>(c.bytes >> 20), c.why,
            static_cast<unsigned long long>(vram >> 20), c.bytes ? "" : ": no device-local copies");
    return c.bytes;
}

uint32_t presents_now() { return static_cast<uint32_t>(counters().presents.load(std::memory_order_relaxed)); }

bool bit_get(const std::vector<uint64_t>& v, uint64_t i) { return (v[static_cast<size_t>(i >> 6)] >> (i & 63)) & 1; }
void bit_put(std::vector<uint64_t>& v, uint64_t i, bool on) {
    if (on) v[static_cast<size_t>(i >> 6)] |= uint64_t{1} << (i & 63);
    else v[static_cast<size_t>(i >> 6)] &= ~(uint64_t{1} << (i & 63));
}

bool page_valid(const Mirror& m, uint64_t p) { return mem::ww::page_valid(m.ww, p); }

bool range_valid(const Mirror& m, uint64_t first, uint64_t count) { return mem::ww::all_valid(m.ww, first, count); }

Mirror* mirror_for(PoolGpu* pg, MemoryPool* mp) {
    if (pg->mirror || pg->mirror_tried) return pg->mirror;
    pg->mirror_tried = true;
    if (!mp || mp->is_virtual || mp->is_shader_code || mp->dead.load() || pg->failed) return nullptr;
    // Tracked copies change the protection of the CPU's view of the pages. With [nvn] pool_view = false the GPU import is that same view;
    // that combination is not tried: no copy.
    if (!pg->view) return nullptr;
    static const uint64_t budget = mirror_budget();
    if (budget == 0) return nullptr;
    if (g_mirror_bytes + pg->size > budget) {
        NXLOG_I(kTag, "pool %p (%llu MiB) gets no device-local copy: over [nvn] pool_mirror_mb", static_cast<void*>(pg->base),
                static_cast<unsigned long long>(pg->size >> 20));
        return nullptr;
    }
    auto m = std::make_unique<Mirror>();
    // Without tracking of the CPU's writes there is no copy: a copy that misses a write serves old vertices and indices, and after an area
    // load those can point anywhere (an untracked experiment lost the Vulkan device at the first load).
    m->ww = mem::ww::region_create(reinterpret_cast<uint64_t>(pg->base), pg->size);
    if (!m->ww) {
        NXLOG_I(kTag, "pool %p (%llu KiB) gets no device-local copy: its writes cannot be tracked (not in the heap's primary view)",
                static_cast<void*>(pg->base), static_cast<unsigned long long>(pg->size >> 10));
        return nullptr;
    }
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (device().create_buffer(pg->size, usage, gfx::MemoryUse::GpuOnly, &m->buf, "nvn pool mirror") != VK_SUCCESS) {
        NXLOG_W(kTag, "pool %p: device-local copy of %llu MiB could not be made", static_cast<void*>(pg->base), static_cast<unsigned long long>(pg->size >> 20));
        if (m->ww) mem::ww::region_destroy(m->ww);
        return nullptr;
    }
    m->src = pg->buffer;
    m->pool_flags = mp->flags;
    m->npages = pg->size / kPageBytes;
    const size_t words = static_cast<size_t>((m->npages + 63) / 64);
    m->want.assign(words, 0);
    m->copied_at.assign(static_cast<size_t>(m->npages), 0);
    m->used.assign(words, 0);
    m->score.assign(static_cast<size_t>(m->npages), 0);
    m->backoff.assign(static_cast<size_t>(m->npages), 0);
    m->hot_until.assign(static_cast<size_t>(m->npages), 0);
    g_mirror_bytes += pg->size;
    g_st_live_bytes.store(static_cast<int64_t>(g_mirror_bytes), std::memory_order_relaxed);
    NXLOG_I(kTag, "pool %p: device-local copy (%llu KiB, pool flags 0x%x, %s)", static_cast<void*>(pg->base), static_cast<unsigned long long>(pg->size >> 10),
            static_cast<unsigned>(mp->flags), "writes tracked");
    pg->mirror = m.release();
    g_mirrors.push_back(pg->mirror);
    return pg->mirror;
}

// The CPU wrote these pages since their copy: they wait for the next draw that wants them. A copy the CPU overwrote before any draw read it
// was wasted (data the game writes for one frame, a ring buffer): after kHotHits of those in a row the page is hot and stays in the pool for
// a hold that doubles every time it comes back.
void absorb_writes(Mirror& m, uint32_t now) {
    g_written_tmp.clear();
    mem::ww::take_written(m.ww, g_written_tmp);
    m.st_written += static_cast<int64_t>(g_written_tmp.size());
    for (uint64_t p : g_written_tmp) {
        const size_t i = static_cast<size_t>(p);
        bit_put(m.want, p, false);
        if (m.copied_at[i] == 0) continue;                     // never copied: nothing was wasted
        if (bit_get(m.used, p)) {
            m.score[i] = 0;
            m.backoff[i] = 0;
            continue;
        }
        if (++m.score[i] >= kHotHits) {
            if (m.hot_until[i] <= now) g_st_hot.fetch_add(1, std::memory_order_relaxed);
            m.hot_until[i] = now + (kHotHold << m.backoff[i]);
            if (m.backoff[i] < kHotMaxShift) ++m.backoff[i];
            m.score[i] = 0;
        }
    }
}

bool copyable(const Mirror& m, uint64_t i, uint32_t now) {
    return bit_get(m.want, i) && !page_valid(m, i) && m.hot_until[static_cast<size_t>(i)] <= now && !mem::ww::page_banned(m.ww, i);
}

void log_summary(uint32_t now) {
    static uint32_t last = 0;
    if (now - last < 1200) return;
    last = now;
    uint64_t valid = 0, hot = 0;
    for (const Mirror* m : g_mirrors)
        for (uint64_t p = 0; p < m->npages; ++p) {
            valid += page_valid(*m, p) ? 1 : 0;
            hot += m->hot_until[static_cast<size_t>(p)] > now ? 1 : 0;
        }
    NXLOG_I(kTag, "pool mirrors: %zu, %llu MiB, valid %llu MiB, hot %llu KiB; binds to mirrors %lld, to pools %lld; copied %lld MiB; write faults %llu",
            g_mirrors.size(), static_cast<unsigned long long>(g_mirror_bytes >> 20), static_cast<unsigned long long>(valid * kPageBytes >> 20),
            static_cast<unsigned long long>(hot * kPageBytes >> 10), static_cast<long long>(g_st_dev_binds.load()), static_cast<long long>(g_st_host_binds.load()),
            static_cast<long long>(g_st_copied.load() >> 20), static_cast<unsigned long long>(mem::ww::faults()));
    NXLOG_I(kTag, "pool mirror binds (copy/pool/not eligible): vertex %lld/%lld/%lld index %lld/%lld/%lld uniform %lld/%lld/%lld; GPU writes (calls/pages): storage %lld/%lld "
            "stream-out %lld/%lld image copy %lld/%lld update %lld/%lld fill %lld/%lld copy %lld/%lld",
            (long long)g_use_dev[0], (long long)g_use_host[0], (long long)g_use_skip[0], (long long)g_use_dev[1], (long long)g_use_host[1], (long long)g_use_skip[1],
            (long long)g_use_dev[2], (long long)g_use_host[2], (long long)g_use_skip[2], (long long)g_wr_calls[0], (long long)g_wr_pages[0], (long long)g_wr_calls[1],
            (long long)g_wr_pages[1], (long long)g_wr_calls[2], (long long)g_wr_pages[2], (long long)g_wr_calls[3], (long long)g_wr_pages[3], (long long)g_wr_calls[4],
            (long long)g_wr_pages[4], (long long)g_wr_calls[5], (long long)g_wr_pages[5]);
    std::vector<const Mirror*> top(g_mirrors.begin(), g_mirrors.end());
    std::sort(top.begin(), top.end(), [](const Mirror* a, const Mirror* b) { return a->st_copied > b->st_copied; });
    for (size_t i = 0; i < top.size() && i < 6; ++i) {
        uint64_t v = 0;
        for (uint64_t p = 0; p < top[i]->npages; ++p) v += page_valid(*top[i], p) ? 1 : 0;
        NXLOG_I(kTag, "  mirror of %llu KiB (flags 0x%x): valid %llu KiB, copied %lld KiB, written by the CPU after a copy %lld KiB", (unsigned long long)(top[i]->npages * 4),
                top[i]->pool_flags, (unsigned long long)(v * 4), (long long)(top[i]->st_copied * 4), (long long)(top[i]->st_written * 4));
    }
}

struct MirrorGauges {
    MirrorGauges() {
        nxrt::gauge_register("nvn.mirror.bytes", [] { return g_st_live_bytes.load(std::memory_order_relaxed); });
        nxrt::gauge_register("nvn.mirror.copied_mb", [] { return g_st_copied.load(std::memory_order_relaxed) >> 20; });
        nxrt::gauge_register("nvn.mirror.dev_binds", [] { return g_st_dev_binds.load(std::memory_order_relaxed); });
        nxrt::gauge_register("nvn.mirror.host_binds", [] { return g_st_host_binds.load(std::memory_order_relaxed); });
        nxrt::gauge_register("nvn.mirror.hot_pages", [] { return g_st_hot.load(std::memory_order_relaxed); });
    }
    ~MirrorGauges() {
        for (const char* n : {"nvn.mirror.bytes", "nvn.mirror.copied_mb", "nvn.mirror.dev_binds", "nvn.mirror.host_binds", "nvn.mirror.hot_pages"}) nxrt::gauge_unregister(n);
    }
};
MirrorGauges g_mirror_gauges;

}  // namespace

bool mirror_enabled() { return mirror_mode() != MirrorMode::Off; }

BufRef resolve_read(uint64_t gpu_address, uint64_t size, int use) {
    MemoryPool* mp = nullptr;
    PoolGpu* pg = nullptr;
    BufRef out = resolve_in_pool(gpu_address, size, &mp, &pg);
    if (!g_mirror_on) return out;
    if (!out.ok || !pg || !size) { g_use_skip[use % 3]++; return out; }
    const uint64_t first = out.offset / kPageBytes, last = (out.offset + size - 1) / kPageBytes;
    if (last - first + 1 > kMaxBindPages) { g_use_skip[use % 3]++; return out; }
    Mirror* m = mirror_for(pg, mp);
    if (!m || last >= m->npages) { g_use_skip[use % 3]++; return out; }
    if (range_valid(*m, first, last - first + 1)) {
        out.buffer = m->buf.buffer;
        for (uint64_t p = first; p <= last; ++p) bit_put(m->used, p, true);
        g_st_dev_binds.fetch_add(1, std::memory_order_relaxed);
        g_use_dev[use % 3]++;
        return out;
    }
    g_use_host[use % 3]++;
    for (uint64_t p = first; p <= last; ++p) bit_put(m->want, p, true);
    m->any_want = true;
    g_st_host_binds.fetch_add(1, std::memory_order_relaxed);
    return out;
}

bool note_gpu_write(uint64_t gpu_address, uint64_t size, int use) {
    if (g_mirrors.empty() || !size) return false;
    const PoolHit hit = pool_lookup(gpu_address);
    if (!hit.pool || !hit.pool->gpu || !hit.pool->gpu->mirror) return false;
    PoolGpu* pg = hit.pool->gpu;
    Mirror& m = *pg->mirror;
    const uint64_t off = gpu_address - reinterpret_cast<uint64_t>(pg->base);
    if (off >= pg->size) return false;
    const uint64_t first = off / kPageBytes, last = std::min<uint64_t>((off + size - 1) / kPageBytes, m.npages - 1);
    g_wr_calls[use % 6]++;
    g_wr_pages[use % 6] += static_cast<int64_t>(last - first + 1);
    bool stale = false;
    for (uint64_t p = first; p <= last && !stale; ++p) stale = page_valid(m, p);
    mem::ww::invalidate(m.ww, first, last - first + 1);
    for (uint64_t p = first; p <= last; ++p) bit_put(m.want, p, false);
    return stale;
}

void mirror_begin_submit(Frame& f, bool on) {
    g_mirror_on = on && mirror_mode() != MirrorMode::Off;
    if (!g_mirror_on) return;
    const uint32_t now = presents_now();
    log_summary(now);
    uint64_t budget = kCopyBudget;
    bool barrier_done = false;
    for (Mirror* mp : g_mirrors) {
        Mirror& m = *mp;
        absorb_writes(m, now);
        if (!m.any_want) continue;
        m.any_want = false;
        g_copy_tmp.clear();
        uint64_t p = 0;
        while (p < m.npages) {
            if (!m.want[static_cast<size_t>(p >> 6)]) { p = (p | 63) + 1; continue; }     // a word with nothing wanted
            if (!bit_get(m.want, p)) { ++p; continue; }
            if (!copyable(m, p, now)) { bit_put(m.want, p, false); ++p; continue; }     // valid already, hot or banned: the next draw asks again
            if (budget < kPageBytes) { m.any_want = true; break; }                      // the rest waits for the next submission
            uint64_t q = p + 1;
            while (q < m.npages && copyable(m, q, now) && (q - p + 1) * kPageBytes <= budget) ++q;
            const bool armed = mem::ww::arm(m.ww, p, q - p);
            for (uint64_t i = p; i < q; ++i) {
                bit_put(m.want, i, false);
                if (!armed) continue;
                m.copied_at[static_cast<size_t>(i)] = now ? now : 1;
                bit_put(m.used, i, false);
            }
            if (armed) {
                g_copy_tmp.push_back(VkBufferCopy{p * kPageBytes, p * kPageBytes, (q - p) * kPageBytes});
                budget -= (q - p) * kPageBytes;
                m.st_copied += static_cast<int64_t>(q - p);
                g_st_copied.fetch_add(static_cast<int64_t>((q - p) * kPageBytes), std::memory_order_relaxed);
            }
            p = q;
        }
        if (g_copy_tmp.empty()) continue;
        if (!barrier_done) {
            memory_barrier(f.cb);      // earlier GPU writes into the pools, and draws still reading the mirrors, come first
            barrier_done = true;
        }
        // the source is the pool's own buffer: the copy reads what the CPU and earlier GPU work left in the pages
        vkCmdCopyBuffer(f.cb, m.src, m.buf.buffer, static_cast<uint32_t>(g_copy_tmp.size()), g_copy_tmp.data());
    }
    if (barrier_done) {
        VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(f.cb, &di);
        if (gpass_on()) gpass_stamp(f, "mirror", 0);      // GPU pass profiler: the copies of this submission
    }
}

void mirror_destroy(PoolGpu* p) {
    Mirror* m = p->mirror;
    if (!m) return;
    p->mirror = nullptr;
    g_mirrors.erase(std::remove(g_mirrors.begin(), g_mirrors.end(), m), g_mirrors.end());
    if (m->ww) mem::ww::region_destroy(m->ww);
    if (m->buf.buffer) device().destroy_buffer(m->buf);
    g_mirror_bytes -= std::min<uint64_t>(g_mirror_bytes, p->size);
    g_st_live_bytes.store(static_cast<int64_t>(g_mirror_bytes), std::memory_order_relaxed);
    delete m;
}

}  // namespace gpu
}  // namespace nxrt::nvn::be
