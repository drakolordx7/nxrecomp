// SPDX-License-Identifier: GPL-3.0-or-later
// NVNmemoryPool, NVNbuffer, NVNtexturePool and NVNsamplerPool through the typed thunks (the memory family, api_memory.cpp).
// Every function is called through abi::proc, the way a game calls it. Pool storage is 4096 aligned host memory; textures are
// host objects built by hand (no GPU: the unit test links nvn_gpu_stub.cpp).
#include <malloc.h>

#include <cstring>

#include "nvn_core.h"
#include "nvn_impl.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_memory)

using namespace nxrt;
using namespace nxrt::nvn;

namespace {

constexpr size_t kRegion = 1u << 20;   // 1 MiB of pool storage

void setup() {
    install();
    abi::reset_mismatches();
    register_api_builders();   // the memory pool and buffer builders the tests set up pools and buffers with
    register_api_sampler();    // the sampler builder and sampler objects
    register_api_memory();
    CHECK_EQ(abi::mismatches(), 0u);
}

// 4096-aligned host storage for a pool, freed when the test ends.
struct Region {
    uint8_t* p;
    explicit Region(size_t n) : p(static_cast<uint8_t*>(_aligned_malloc(n, 4096))) { std::memset(p, 0, n); }
    ~Region() { _aligned_free(p); }
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;
};

uint64_t addr_of(const void* p) { return reinterpret_cast<uint64_t>(p); }

}  // namespace

NXTEST(all_entry_points_registered) {
    setup();
    const char* names[] = {
        // memory pool: 10
        "nvnMemoryPoolInitialize", "nvnMemoryPoolSetDebugLabel", "nvnMemoryPoolFinalize", "nvnMemoryPoolMap",
        "nvnMemoryPoolFlushMappedRange", "nvnMemoryPoolInvalidateMappedRange", "nvnMemoryPoolGetBufferAddress",
        "nvnMemoryPoolMapVirtual", "nvnMemoryPoolGetSize", "nvnMemoryPoolGetFlags",
        // buffer: 11
        "nvnBufferInitialize", "nvnBufferSetDebugLabel", "nvnBufferFinalize", "nvnBufferMap", "nvnBufferGetAddress",
        "nvnBufferFlushMappedRange", "nvnBufferInvalidateMappedRange", "nvnBufferGetMemoryPool",
        "nvnBufferGetMemoryOffset", "nvnBufferGetSize", "nvnBufferGetDebugID",
        // texture pool: 8
        "nvnTexturePoolInitialize", "nvnTexturePoolSetDebugLabel", "nvnTexturePoolFinalize",
        "nvnTexturePoolRegisterTexture", "nvnTexturePoolRegisterImage", "nvnTexturePoolGetMemoryPool",
        "nvnTexturePoolGetMemoryOffset", "nvnTexturePoolGetSize",
        // sampler pool: 8
        "nvnSamplerPoolInitialize", "nvnSamplerPoolSetDebugLabel", "nvnSamplerPoolFinalize",
        "nvnSamplerPoolRegisterSampler", "nvnSamplerPoolRegisterSamplerBuilder", "nvnSamplerPoolGetMemoryPool",
        "nvnSamplerPoolGetMemoryOffset", "nvnSamplerPoolGetSize",
    };
    for (const char* n : names) CHECK(is_implemented(n));
    CHECK_EQ(sizeof(names) / sizeof(names[0]), size_t(37));
    CHECK_EQ(abi::mismatches(), 0u);
}

NXTEST(memory_pool_lifecycle_and_lookup) {
    setup();
    auto b_defaults = abi::proc<PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC>("nvnMemoryPoolBuilderSetDefaults");
    auto b_storage = abi::proc<PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC>("nvnMemoryPoolBuilderSetStorage");
    auto b_flags = abi::proc<PFNNVNMEMORYPOOLBUILDERSETFLAGSPROC>("nvnMemoryPoolBuilderSetFlags");
    auto init = abi::proc<PFNNVNMEMORYPOOLINITIALIZEPROC>("nvnMemoryPoolInitialize");
    auto label = abi::proc<PFNNVNMEMORYPOOLSETDEBUGLABELPROC>("nvnMemoryPoolSetDebugLabel");
    auto map = abi::proc<PFNNVNMEMORYPOOLMAPPROC>("nvnMemoryPoolMap");
    auto flush = abi::proc<PFNNVNMEMORYPOOLFLUSHMAPPEDRANGEPROC>("nvnMemoryPoolFlushMappedRange");
    auto inval = abi::proc<PFNNVNMEMORYPOOLINVALIDATEMAPPEDRANGEPROC>("nvnMemoryPoolInvalidateMappedRange");
    auto get_addr = abi::proc<PFNNVNMEMORYPOOLGETBUFFERADDRESSPROC>("nvnMemoryPoolGetBufferAddress");
    auto get_size = abi::proc<PFNNVNMEMORYPOOLGETSIZEPROC>("nvnMemoryPoolGetSize");
    auto get_flags = abi::proc<PFNNVNMEMORYPOOLGETFLAGSPROC>("nvnMemoryPoolGetFlags");
    auto fin = abi::proc<PFNNVNMEMORYPOOLFINALIZEPROC>("nvnMemoryPoolFinalize");

    Region mem(kRegion);
    const int flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_SHADER_CODE;
    NVNmemoryPoolBuilder b;
    b_defaults(&b);
    b_storage(&b, mem.p, kRegion);
    b_flags(&b, flags);
    NVNmemoryPool pool;
    std::memset(&pool, 0, sizeof pool);
    CHECK_EQ(init(&pool, &b), 1);

    CHECK_EQ(get_size(&pool), kRegion);
    CHECK_EQ(static_cast<int>(get_flags(&pool)), flags);
    CHECK(map(&pool) == static_cast<void*>(mem.p));
    CHECK_EQ(get_addr(&pool), addr_of(mem.p));   // non-virtual: the GPU address is the storage address

    // The pool is found by its GPU address range, with the right offset, and not beyond its end.
    be::MemoryPool* host = be::host_of<be::MemoryPool>(&pool);
    CHECK(host != nullptr);
    CHECK(host != nullptr && host->is_shader_code);
    CHECK(host != nullptr && host->base == mem.p);
    const be::PoolHit hit = be::pool_lookup(addr_of(mem.p) + 4096);
    CHECK(hit.pool == host);
    CHECK_EQ(hit.offset, uint64_t(4096));
    CHECK(be::pool_lookup(addr_of(mem.p) + kRegion).pool == nullptr);

    label(&pool, "unit test pool");
    flush(&pool, 0, 64);      // no textures here: must not crash
    inval(&pool, 0, 64);      // no-op

    fin(&pool);
    CHECK(be::pool_lookup(addr_of(mem.p) + 4096).pool == nullptr);   // gone after Finalize
    CHECK(map(&pool) == nullptr);
    CHECK_EQ(get_size(&pool), size_t(0));
}

NXTEST(memory_pool_rejects_bad_builders_and_accepts_unaligned) {
    setup();
    auto b_defaults = abi::proc<PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC>("nvnMemoryPoolBuilderSetDefaults");
    auto b_storage = abi::proc<PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC>("nvnMemoryPoolBuilderSetStorage");
    auto b_flags = abi::proc<PFNNVNMEMORYPOOLBUILDERSETFLAGSPROC>("nvnMemoryPoolBuilderSetFlags");
    auto init = abi::proc<PFNNVNMEMORYPOOLINITIALIZEPROC>("nvnMemoryPoolInitialize");
    auto get_size = abi::proc<PFNNVNMEMORYPOOLGETSIZEPROC>("nvnMemoryPoolGetSize");
    auto fin = abi::proc<PFNNVNMEMORYPOOLFINALIZEPROC>("nvnMemoryPoolFinalize");

    Region mem(kRegion);
    NVNmemoryPoolBuilder b;
    b_defaults(&b);
    NVNmemoryPool pool;
    std::memset(&pool, 0, sizeof pool);

    b_storage(&b, mem.p, 0);                                  // size 0
    CHECK_EQ(init(&pool, &b), 0);
    b_storage(&b, nullptr, kRegion);                          // non-virtual pool without storage
    CHECK_EQ(init(&pool, &b), 0);
    CHECK(be::host_of<be::MemoryPool>(&pool) == nullptr);

    b_storage(&b, mem.p + 16, kRegion - 16);                  // not 4096 aligned: accepted (with a one-time warning)
    CHECK_EQ(init(&pool, &b), 1);
    CHECK_EQ(get_size(&pool), kRegion - 16);
    fin(&pool);

    b_flags(&b, NVN_MEMORY_POOL_FLAGS_VIRTUAL);               // virtual pool: storage is not needed
    b_storage(&b, nullptr, kRegion);
    CHECK_EQ(init(&pool, &b), 1);
    CHECK_EQ(get_size(&pool), kRegion);
    fin(&pool);
}

NXTEST(virtual_pools_get_distinct_synthetic_ranges) {
    setup();
    auto b_defaults = abi::proc<PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC>("nvnMemoryPoolBuilderSetDefaults");
    auto b_storage = abi::proc<PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC>("nvnMemoryPoolBuilderSetStorage");
    auto b_flags = abi::proc<PFNNVNMEMORYPOOLBUILDERSETFLAGSPROC>("nvnMemoryPoolBuilderSetFlags");
    auto init = abi::proc<PFNNVNMEMORYPOOLINITIALIZEPROC>("nvnMemoryPoolInitialize");
    auto map = abi::proc<PFNNVNMEMORYPOOLMAPPROC>("nvnMemoryPoolMap");
    auto flush = abi::proc<PFNNVNMEMORYPOOLFLUSHMAPPEDRANGEPROC>("nvnMemoryPoolFlushMappedRange");
    auto get_addr = abi::proc<PFNNVNMEMORYPOOLGETBUFFERADDRESSPROC>("nvnMemoryPoolGetBufferAddress");
    auto get_flags = abi::proc<PFNNVNMEMORYPOOLGETFLAGSPROC>("nvnMemoryPoolGetFlags");
    auto map_virtual = abi::proc<PFNNVNMEMORYPOOLMAPVIRTUALPROC>("nvnMemoryPoolMapVirtual");
    auto fin = abi::proc<PFNNVNMEMORYPOOLFINALIZEPROC>("nvnMemoryPoolFinalize");

    NVNmemoryPoolBuilder b1, b2;
    b_defaults(&b1);
    b_storage(&b1, nullptr, 1u << 20);
    b_flags(&b1, NVN_MEMORY_POOL_FLAGS_VIRTUAL);
    b_defaults(&b2);
    b_storage(&b2, nullptr, 100u << 10);
    b_flags(&b2, NVN_MEMORY_POOL_FLAGS_VIRTUAL);

    NVNmemoryPool a, c;
    std::memset(&a, 0, sizeof a);
    std::memset(&c, 0, sizeof c);
    CHECK_EQ(init(&a, &b1), 1);
    CHECK_EQ(init(&c, &b2), 1);

    const uint64_t aa = get_addr(&a), ca = get_addr(&c);
    CHECK(aa >= 0x0000200000000000ull);
    CHECK(aa % (64u << 10) == 0);
    CHECK(ca % (64u << 10) == 0);
    CHECK(ca >= aa + (1u << 20));                             // the ranges do not overlap
    CHECK(be::pool_lookup(aa + 4096).pool == be::host_of<be::MemoryPool>(&a));
    CHECK(be::pool_lookup(ca + 4096).pool == be::host_of<be::MemoryPool>(&c));
    CHECK(static_cast<int>(get_flags(&a)) & NVN_MEMORY_POOL_FLAGS_VIRTUAL);

    CHECK(map(&a) == nullptr);                                // nothing is mapped for a virtual pool
    flush(&a, 0, 4096);                                       // no storage: nothing to mark, no crash
    CHECK_EQ(map_virtual(&a, 0, nullptr), 0);                 // virtual mapping is not supported

    fin(&a);
    fin(&c);
    CHECK(be::pool_lookup(aa + 4096).pool == nullptr);
}

NXTEST(buffer_address_map_and_flush) {
    setup();
    auto p_defaults = abi::proc<PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC>("nvnMemoryPoolBuilderSetDefaults");
    auto p_storage = abi::proc<PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC>("nvnMemoryPoolBuilderSetStorage");
    auto p_flags = abi::proc<PFNNVNMEMORYPOOLBUILDERSETFLAGSPROC>("nvnMemoryPoolBuilderSetFlags");
    auto p_init = abi::proc<PFNNVNMEMORYPOOLINITIALIZEPROC>("nvnMemoryPoolInitialize");
    auto p_fin = abi::proc<PFNNVNMEMORYPOOLFINALIZEPROC>("nvnMemoryPoolFinalize");
    auto p_flush = abi::proc<PFNNVNMEMORYPOOLFLUSHMAPPEDRANGEPROC>("nvnMemoryPoolFlushMappedRange");
    auto bb_defaults = abi::proc<PFNNVNBUFFERBUILDERSETDEFAULTSPROC>("nvnBufferBuilderSetDefaults");
    auto bb_storage = abi::proc<PFNNVNBUFFERBUILDERSETSTORAGEPROC>("nvnBufferBuilderSetStorage");
    auto init = abi::proc<PFNNVNBUFFERINITIALIZEPROC>("nvnBufferInitialize");
    auto label = abi::proc<PFNNVNBUFFERSETDEBUGLABELPROC>("nvnBufferSetDebugLabel");
    auto fin = abi::proc<PFNNVNBUFFERFINALIZEPROC>("nvnBufferFinalize");
    auto map = abi::proc<PFNNVNBUFFERMAPPROC>("nvnBufferMap");
    auto get_addr = abi::proc<PFNNVNBUFFERGETADDRESSPROC>("nvnBufferGetAddress");
    auto flush = abi::proc<PFNNVNBUFFERFLUSHMAPPEDRANGEPROC>("nvnBufferFlushMappedRange");
    auto inval = abi::proc<PFNNVNBUFFERINVALIDATEMAPPEDRANGEPROC>("nvnBufferInvalidateMappedRange");
    auto get_pool = abi::proc<PFNNVNBUFFERGETMEMORYPOOLPROC>("nvnBufferGetMemoryPool");
    auto get_off = abi::proc<PFNNVNBUFFERGETMEMORYOFFSETPROC>("nvnBufferGetMemoryOffset");
    auto get_size = abi::proc<PFNNVNBUFFERGETSIZEPROC>("nvnBufferGetSize");
    auto get_id = abi::proc<PFNNVNBUFFERGETDEBUGIDPROC>("nvnBufferGetDebugID");

    Region mem(kRegion);
    NVNmemoryPoolBuilder pb;
    p_defaults(&pb);
    p_storage(&pb, mem.p, kRegion);
    p_flags(&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED);
    NVNmemoryPool pool;
    std::memset(&pool, 0, sizeof pool);
    CHECK_EQ(p_init(&pool, &pb), 1);

    NVNbufferBuilder bb;
    bb_defaults(&bb);
    bb_storage(&bb, &pool, 4096, 256);
    NVNbuffer buf;
    std::memset(&buf, 0, sizeof buf);
    CHECK_EQ(init(&buf, &bb), 1);
    CHECK_EQ(get_addr(&buf), addr_of(mem.p + 4096));
    CHECK_EQ(get_size(&buf), size_t(256));
    CHECK(get_pool(&buf) == &pool);
    CHECK_EQ(get_off(&buf), ptrdiff_t(4096));
    CHECK(get_id(&buf) == (addr_of(mem.p + 4096) & 0xffffffffull));
    CHECK_EQ(get_id(&buf), get_id(&buf));                       // stable

    // Map gives the host pointer of the buffer, and writes through it are visible in the pool storage.
    uint8_t* host = static_cast<uint8_t*>(map(&buf));
    CHECK(host == mem.p + 4096);
    host[0] = 0xAB;
    CHECK_EQ(static_cast<int>(mem.p[4096]), 0xAB);
    label(&buf, "unit test buffer");
    inval(&buf, 0, 256);                                        // no-op

    // A flush marks the textures in its range as CPU-written, and only those.
    be::Texture* tex = new be::Texture();
    tex->mem = mem.p + 4096 + 64;
    tex->storage_size = 128;
    tex->cpu_dirty.store(false);
    be::texture_registry_add(tex);
    NVNtexture tex_guest;
    std::memset(&tex_guest, 0, sizeof tex_guest);
    be::bind_guest(&tex_guest, tex);

    flush(&buf, 100, 10);                                       // buffer bytes 100..110 are inside the texture
    CHECK(tex->cpu_dirty.load());
    tex->cpu_dirty.store(false);
    flush(&buf, 0, 16);                                         // bytes 0..16 are before the texture
    CHECK(!tex->cpu_dirty.load());
    p_flush(&pool, 4096 + 100, 10);                             // the pool flush marks it too
    CHECK(tex->cpu_dirty.load());

    be::texture_registry_remove(tex);
    be::unbind_guest(&tex_guest);                                   // the texture is freed here

    fin(&buf);
    CHECK_EQ(get_size(&buf), size_t(0));
    CHECK(map(&buf) == nullptr);
    p_fin(&pool);
}

NXTEST(buffer_rejects_ranges_outside_the_pool) {
    setup();
    auto p_defaults = abi::proc<PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC>("nvnMemoryPoolBuilderSetDefaults");
    auto p_storage = abi::proc<PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC>("nvnMemoryPoolBuilderSetStorage");
    auto p_init = abi::proc<PFNNVNMEMORYPOOLINITIALIZEPROC>("nvnMemoryPoolInitialize");
    auto p_fin = abi::proc<PFNNVNMEMORYPOOLFINALIZEPROC>("nvnMemoryPoolFinalize");
    auto bb_defaults = abi::proc<PFNNVNBUFFERBUILDERSETDEFAULTSPROC>("nvnBufferBuilderSetDefaults");
    auto bb_storage = abi::proc<PFNNVNBUFFERBUILDERSETSTORAGEPROC>("nvnBufferBuilderSetStorage");
    auto init = abi::proc<PFNNVNBUFFERINITIALIZEPROC>("nvnBufferInitialize");
    auto get_size = abi::proc<PFNNVNBUFFERGETSIZEPROC>("nvnBufferGetSize");
    auto fin = abi::proc<PFNNVNBUFFERFINALIZEPROC>("nvnBufferFinalize");

    Region mem(kRegion);
    NVNmemoryPoolBuilder pb;
    p_defaults(&pb);
    p_storage(&pb, mem.p, kRegion);
    NVNmemoryPool pool;
    std::memset(&pool, 0, sizeof pool);
    CHECK_EQ(p_init(&pool, &pb), 1);

    NVNbufferBuilder bb;
    NVNbuffer buf;
    std::memset(&buf, 0, sizeof buf);
    bb_defaults(&bb);
    bb_storage(&bb, &pool, static_cast<ptrdiff_t>(kRegion - 100), 200);     // runs past the end
    CHECK_EQ(init(&buf, &bb), 0);
    bb_storage(&bb, &pool, -1, 16);                                          // negative offset
    CHECK_EQ(init(&buf, &bb), 0);
    bb_storage(&bb, &pool, 0, kRegion);                                      // exactly the whole pool: fine
    CHECK_EQ(init(&buf, &bb), 1);
    CHECK_EQ(get_size(&buf), kRegion);
    fin(&buf);

    NVNmemoryPool not_a_pool;                                                // a guest object that is not a pool of ours
    std::memset(&not_a_pool, 0, sizeof not_a_pool);
    bb_storage(&bb, &not_a_pool, 0, 16);
    CHECK_EQ(init(&buf, &bb), 0);
    p_fin(&pool);
}

NXTEST(buffer_copy_stays_valid) {
    setup();
    auto p_defaults = abi::proc<PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC>("nvnMemoryPoolBuilderSetDefaults");
    auto p_storage = abi::proc<PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC>("nvnMemoryPoolBuilderSetStorage");
    auto p_init = abi::proc<PFNNVNMEMORYPOOLINITIALIZEPROC>("nvnMemoryPoolInitialize");
    auto p_fin = abi::proc<PFNNVNMEMORYPOOLFINALIZEPROC>("nvnMemoryPoolFinalize");
    auto bb_defaults = abi::proc<PFNNVNBUFFERBUILDERSETDEFAULTSPROC>("nvnBufferBuilderSetDefaults");
    auto bb_storage = abi::proc<PFNNVNBUFFERBUILDERSETSTORAGEPROC>("nvnBufferBuilderSetStorage");
    auto init = abi::proc<PFNNVNBUFFERINITIALIZEPROC>("nvnBufferInitialize");
    auto fin = abi::proc<PFNNVNBUFFERFINALIZEPROC>("nvnBufferFinalize");
    auto map = abi::proc<PFNNVNBUFFERMAPPROC>("nvnBufferMap");
    auto get_addr = abi::proc<PFNNVNBUFFERGETADDRESSPROC>("nvnBufferGetAddress");
    auto get_size = abi::proc<PFNNVNBUFFERGETSIZEPROC>("nvnBufferGetSize");

    Region mem(kRegion);
    NVNmemoryPoolBuilder pb;
    p_defaults(&pb);
    p_storage(&pb, mem.p, kRegion);
    NVNmemoryPool pool;
    std::memset(&pool, 0, sizeof pool);
    CHECK_EQ(p_init(&pool, &pb), 1);

    NVNbufferBuilder bb;
    bb_defaults(&bb);
    bb_storage(&bb, &pool, 8192, 256);
    NVNbuffer a;
    std::memset(&a, 0, sizeof a);
    CHECK_EQ(init(&a, &bb), 1);

    NVNbuffer copy;                                                          // the game copies buffer objects around
    std::memcpy(&copy, &a, sizeof a);
    fin(&a);                                                                 // the original is gone, the copy still works
    CHECK_EQ(get_addr(&a), uint64_t(0));
    CHECK_EQ(get_size(&copy), size_t(256));
    CHECK_EQ(get_addr(&copy), addr_of(mem.p + 8192));
    CHECK(static_cast<uint8_t*>(map(&copy)) == mem.p + 8192);
    p_fin(&pool);
}

NXTEST(texture_pool_register_and_get) {
    setup();
    auto p_defaults = abi::proc<PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC>("nvnMemoryPoolBuilderSetDefaults");
    auto p_storage = abi::proc<PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC>("nvnMemoryPoolBuilderSetStorage");
    auto p_init = abi::proc<PFNNVNMEMORYPOOLINITIALIZEPROC>("nvnMemoryPoolInitialize");
    auto p_fin = abi::proc<PFNNVNMEMORYPOOLFINALIZEPROC>("nvnMemoryPoolFinalize");
    auto tp_init = abi::proc<PFNNVNTEXTUREPOOLINITIALIZEPROC>("nvnTexturePoolInitialize");
    auto tp_label = abi::proc<PFNNVNTEXTUREPOOLSETDEBUGLABELPROC>("nvnTexturePoolSetDebugLabel");
    auto tp_fin = abi::proc<PFNNVNTEXTUREPOOLFINALIZEPROC>("nvnTexturePoolFinalize");
    auto reg_tex = abi::proc<PFNNVNTEXTUREPOOLREGISTERTEXTUREPROC>("nvnTexturePoolRegisterTexture");
    auto reg_img = abi::proc<PFNNVNTEXTUREPOOLREGISTERIMAGEPROC>("nvnTexturePoolRegisterImage");
    auto tp_mem = abi::proc<PFNNVNTEXTUREPOOLGETMEMORYPOOLPROC>("nvnTexturePoolGetMemoryPool");
    auto tp_off = abi::proc<PFNNVNTEXTUREPOOLGETMEMORYOFFSETPROC>("nvnTexturePoolGetMemoryOffset");
    auto tp_size = abi::proc<PFNNVNTEXTUREPOOLGETSIZEPROC>("nvnTexturePoolGetSize");

    Region mem(kRegion);
    NVNmemoryPoolBuilder pb;
    p_defaults(&pb);
    p_storage(&pb, mem.p, kRegion);
    NVNmemoryPool pool;
    std::memset(&pool, 0, sizeof pool);
    CHECK_EQ(p_init(&pool, &pb), 1);

    NVNtexturePool tp;
    std::memset(&tp, 0, sizeof tp);
    CHECK_EQ(tp_init(&tp, &pool, 4096, 16), 1);
    CHECK_EQ(tp_size(&tp), 16);
    CHECK(tp_mem(&tp) == &pool);
    CHECK_EQ(tp_off(&tp), ptrdiff_t(4096));
    tp_label(&tp, "unit test texture pool");

    be::TexturePool* host = be::host_of<be::TexturePool>(&tp);
    CHECK(host != nullptr);
    CHECK(host != nullptr && host->entries.size() == 16);

    be::Texture* t = new be::Texture();
    t->mem = mem.p + 8192;
    t->storage_size = 4096;
    NVNtexture tex_guest;
    std::memset(&tex_guest, 0, sizeof tex_guest);
    be::bind_guest(&tex_guest, t);                                  // the guest object holds one reference

    NVNtextureView view;
    std::memset(&view, 0, sizeof view);
    reg_tex(&tp, 3, &tex_guest, &view);                         // a view was given
    reg_img(&tp, 5, &tex_guest, nullptr);                       // an image without a view
    if (host) {
        CHECK(host->entries[3].tex.get() == t);
        CHECK(host->entries[3].used);
        CHECK(host->entries[3].has_view);
        CHECK(host->entries[5].tex.get() == t);
        CHECK(host->entries[5].used);
        CHECK(!host->entries[5].has_view);
        CHECK(!host->entries[4].used);
        CHECK_EQ(t->refs.load(), 3);                            // guest object + two pool entries
    }
    reg_tex(&tp, 16, &tex_guest, nullptr);                      // ids outside the pool are ignored
    reg_img(&tp, -1, &tex_guest, nullptr);
    if (host) CHECK_EQ(host->entries.size(), size_t(16));

    tp_fin(&tp);
    CHECK_EQ(tp_size(&tp), 0);
    CHECK(tp_mem(&tp) == nullptr);
    CHECK_EQ(t->refs.load(), 1);                                // the pool entries are gone
    be::unbind_guest(&tex_guest);                                   // the texture is freed here
    p_fin(&pool);
}

NXTEST(sampler_pool_stores_decoded_samplers) {
    setup();
    auto p_defaults = abi::proc<PFNNVNMEMORYPOOLBUILDERSETDEFAULTSPROC>("nvnMemoryPoolBuilderSetDefaults");
    auto p_storage = abi::proc<PFNNVNMEMORYPOOLBUILDERSETSTORAGEPROC>("nvnMemoryPoolBuilderSetStorage");
    auto p_init = abi::proc<PFNNVNMEMORYPOOLINITIALIZEPROC>("nvnMemoryPoolInitialize");
    auto p_fin = abi::proc<PFNNVNMEMORYPOOLFINALIZEPROC>("nvnMemoryPoolFinalize");
    auto sb_defaults = abi::proc<PFNNVNSAMPLERBUILDERSETDEFAULTSPROC>("nvnSamplerBuilderSetDefaults");
    auto sb_minmag = abi::proc<PFNNVNSAMPLERBUILDERSETMINMAGFILTERPROC>("nvnSamplerBuilderSetMinMagFilter");
    auto sb_aniso = abi::proc<PFNNVNSAMPLERBUILDERSETMAXANISOTROPYPROC>("nvnSamplerBuilderSetMaxAnisotropy");
    auto s_init = abi::proc<PFNNVNSAMPLERINITIALIZEPROC>("nvnSamplerInitialize");
    auto sp_init = abi::proc<PFNNVNSAMPLERPOOLINITIALIZEPROC>("nvnSamplerPoolInitialize");
    auto sp_label = abi::proc<PFNNVNSAMPLERPOOLSETDEBUGLABELPROC>("nvnSamplerPoolSetDebugLabel");
    auto sp_fin = abi::proc<PFNNVNSAMPLERPOOLFINALIZEPROC>("nvnSamplerPoolFinalize");
    auto sp_reg = abi::proc<PFNNVNSAMPLERPOOLREGISTERSAMPLERPROC>("nvnSamplerPoolRegisterSampler");
    auto sp_reg_b = abi::proc<PFNNVNSAMPLERPOOLREGISTERSAMPLERBUILDERPROC>("nvnSamplerPoolRegisterSamplerBuilder");
    auto sp_mem = abi::proc<PFNNVNSAMPLERPOOLGETMEMORYPOOLPROC>("nvnSamplerPoolGetMemoryPool");
    auto sp_off = abi::proc<PFNNVNSAMPLERPOOLGETMEMORYOFFSETPROC>("nvnSamplerPoolGetMemoryOffset");
    auto sp_size = abi::proc<PFNNVNSAMPLERPOOLGETSIZEPROC>("nvnSamplerPoolGetSize");

    Region mem(kRegion);
    NVNmemoryPoolBuilder pb;
    p_defaults(&pb);
    p_storage(&pb, mem.p, kRegion);
    NVNmemoryPool pool;
    std::memset(&pool, 0, sizeof pool);
    CHECK_EQ(p_init(&pool, &pb), 1);

    NVNsamplerBuilder sb;
    sb_defaults(&sb);
    sb_minmag(&sb, NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR, NVN_MAG_FILTER_LINEAR);
    sb_aniso(&sb, 4.0f);
    const SamplerInfo want = decode(&sb);

    NVNsamplerPool sp;
    std::memset(&sp, 0, sizeof sp);
    CHECK_EQ(sp_init(&sp, &pool, 0, 8), 1);
    CHECK_EQ(sp_size(&sp), 8);
    CHECK(sp_mem(&sp) == &pool);
    CHECK_EQ(sp_off(&sp), ptrdiff_t(0));
    sp_label(&sp, "unit test sampler pool");

    sp_reg_b(&sp, 2, &sb);                                      // registered from a builder
    be::SamplerPool* host = be::host_of<be::SamplerPool>(&sp);
    CHECK(host != nullptr);
    if (host) {
        CHECK(host->entries[2] == want);
        CHECK(host->used[2] == 1);
        CHECK(host->used[1] == 0);
    }

    NVNsampler s;
    std::memset(&s, 0, sizeof s);
    CHECK_EQ(s_init(&s, &sb), 1);
    CHECK(decode(&s) == want);
    sp_reg(&sp, 4, &s);                                         // registered from a sampler object
    if (host) {
        CHECK(host->entries[4] == want);
        CHECK(host->used[4] == 1);
    }
    sp_reg(&sp, 8, &s);                                         // outside the pool: ignored
    sp_reg_b(&sp, -1, &sb);
    if (host) CHECK_EQ(host->used.size(), size_t(8));

    sp_fin(&sp);
    CHECK_EQ(sp_size(&sp), 0);
    CHECK(sp_mem(&sp) == nullptr);
    p_fin(&pool);
}

NXTEST(objects_that_are_not_ours_return_zero) {
    setup();
    auto p_size = abi::proc<PFNNVNMEMORYPOOLGETSIZEPROC>("nvnMemoryPoolGetSize");
    auto p_flags = abi::proc<PFNNVNMEMORYPOOLGETFLAGSPROC>("nvnMemoryPoolGetFlags");
    auto p_map = abi::proc<PFNNVNMEMORYPOOLMAPPROC>("nvnMemoryPoolMap");
    auto p_addr = abi::proc<PFNNVNMEMORYPOOLGETBUFFERADDRESSPROC>("nvnMemoryPoolGetBufferAddress");
    auto p_mapv = abi::proc<PFNNVNMEMORYPOOLMAPVIRTUALPROC>("nvnMemoryPoolMapVirtual");
    auto p_flush = abi::proc<PFNNVNMEMORYPOOLFLUSHMAPPEDRANGEPROC>("nvnMemoryPoolFlushMappedRange");
    auto p_fin = abi::proc<PFNNVNMEMORYPOOLFINALIZEPROC>("nvnMemoryPoolFinalize");
    auto b_init = abi::proc<PFNNVNBUFFERINITIALIZEPROC>("nvnBufferInitialize");
    auto b_bb_defaults = abi::proc<PFNNVNBUFFERBUILDERSETDEFAULTSPROC>("nvnBufferBuilderSetDefaults");
    auto b_bb_storage = abi::proc<PFNNVNBUFFERBUILDERSETSTORAGEPROC>("nvnBufferBuilderSetStorage");
    auto b_addr = abi::proc<PFNNVNBUFFERGETADDRESSPROC>("nvnBufferGetAddress");
    auto b_map = abi::proc<PFNNVNBUFFERMAPPROC>("nvnBufferMap");
    auto b_pool = abi::proc<PFNNVNBUFFERGETMEMORYPOOLPROC>("nvnBufferGetMemoryPool");
    auto b_off = abi::proc<PFNNVNBUFFERGETMEMORYOFFSETPROC>("nvnBufferGetMemoryOffset");
    auto b_size = abi::proc<PFNNVNBUFFERGETSIZEPROC>("nvnBufferGetSize");
    auto b_id = abi::proc<PFNNVNBUFFERGETDEBUGIDPROC>("nvnBufferGetDebugID");
    auto b_flush = abi::proc<PFNNVNBUFFERFLUSHMAPPEDRANGEPROC>("nvnBufferFlushMappedRange");
    auto b_fin = abi::proc<PFNNVNBUFFERFINALIZEPROC>("nvnBufferFinalize");
    auto tp_init = abi::proc<PFNNVNTEXTUREPOOLINITIALIZEPROC>("nvnTexturePoolInitialize");
    auto tp_fin = abi::proc<PFNNVNTEXTUREPOOLFINALIZEPROC>("nvnTexturePoolFinalize");
    auto tp_reg = abi::proc<PFNNVNTEXTUREPOOLREGISTERTEXTUREPROC>("nvnTexturePoolRegisterTexture");
    auto tp_mem = abi::proc<PFNNVNTEXTUREPOOLGETMEMORYPOOLPROC>("nvnTexturePoolGetMemoryPool");
    auto tp_size = abi::proc<PFNNVNTEXTUREPOOLGETSIZEPROC>("nvnTexturePoolGetSize");
    auto sp_init = abi::proc<PFNNVNSAMPLERPOOLINITIALIZEPROC>("nvnSamplerPoolInitialize");
    auto sp_fin = abi::proc<PFNNVNSAMPLERPOOLFINALIZEPROC>("nvnSamplerPoolFinalize");
    auto sp_size = abi::proc<PFNNVNSAMPLERPOOLGETSIZEPROC>("nvnSamplerPoolGetSize");
    auto sp_mem = abi::proc<PFNNVNSAMPLERPOOLGETMEMORYPOOLPROC>("nvnSamplerPoolGetMemoryPool");

    NVNmemoryPool zp;
    std::memset(&zp, 0, sizeof zp);
    CHECK_EQ(p_size(&zp), size_t(0));
    CHECK_EQ(static_cast<int>(p_flags(&zp)), 0);
    CHECK(p_map(&zp) == nullptr);
    CHECK_EQ(p_addr(&zp), uint64_t(0));
    CHECK_EQ(p_mapv(&zp, 0, nullptr), 0);
    p_flush(&zp, 0, 16);
    p_fin(&zp);

    NVNbuffer zb;
    std::memset(&zb, 0, sizeof zb);
    CHECK_EQ(b_addr(&zb), uint64_t(0));
    CHECK(b_map(&zb) == nullptr);
    CHECK(b_pool(&zb) == nullptr);
    CHECK_EQ(b_off(&zb), ptrdiff_t(0));
    CHECK_EQ(b_size(&zb), size_t(0));
    CHECK_EQ(b_id(&zb), uint64_t(0));
    b_flush(&zb, 0, 16);
    b_fin(&zb);

    // A buffer cannot be built over a pool that is not ours.
    NVNbufferBuilder bb;
    b_bb_defaults(&bb);
    b_bb_storage(&bb, &zp, 0, 16);
    CHECK_EQ(b_init(&zb, &bb), 0);

    NVNtexturePool ztp;
    std::memset(&ztp, 0, sizeof ztp);
    CHECK_EQ(tp_size(&ztp), 0);
    CHECK(tp_mem(&ztp) == nullptr);
    tp_reg(&ztp, 0, nullptr, nullptr);
    tp_fin(&ztp);
    CHECK_EQ(tp_init(&ztp, &zp, 0, 4), 0);

    NVNsamplerPool zsp;
    std::memset(&zsp, 0, sizeof zsp);
    CHECK_EQ(sp_size(&zsp), 0);
    CHECK(sp_mem(&zsp) == nullptr);
    sp_fin(&zsp);
    CHECK_EQ(sp_init(&zsp, &zp, 0, 4), 0);
}
