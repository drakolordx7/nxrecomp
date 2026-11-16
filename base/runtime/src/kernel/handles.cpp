// SPDX-License-Identifier: GPL-3.0-or-later
// Kernel handle table (include/nxrt/kernel.h). Pattern from SuperMarioWonderRecomp runtime/src/kernel.cpp handle_add/get/close (MIT).
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/os/sync.h"

namespace nxrt::kernel {
namespace {
os::SharedMutex g_lock;
std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
uint32_t g_next = 0x10000;
}

uint32_t handle_add(std::shared_ptr<KObject> o) {
    std::lock_guard<os::SharedMutex> lk(g_lock);
    uint32_t h = 0;
    if (g_handles.size() < 0x3000) {          // Horizon's per-process handle table holds 0x400..0x3000 entries
        h = g_next;
        g_next += 1;
        if (g_next >= 0x40000000) g_next = 0x10000;
        g_handles[h] = std::move(o);
    }
    return h;
}

std::shared_ptr<KObject> handle_get(uint32_t h) {
    if (h == kPseudoCurrentThread) {
        KThread* t = current_thread();
        return t ? t->self.lock() : nullptr;
    }
    std::shared_lock<os::SharedMutex> lk(g_lock);
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}

void handle_close(uint32_t h) {
    std::shared_ptr<KObject> dying;   // destroyed outside the lock
    {
        std::lock_guard<os::SharedMutex> lk(g_lock);
        auto it = g_handles.find(h);
        if (it != g_handles.end()) {
            dying = std::move(it->second);
            g_handles.erase(it);
        }
    }
}

size_t handle_count() {
    std::shared_lock<os::SharedMutex> lk(g_lock);
    return g_handles.size();
}

void handles_reset() {
    std::unordered_map<uint32_t, std::shared_ptr<KObject>> old;
    {
        std::lock_guard<os::SharedMutex> lk(g_lock);
        old.swap(g_handles);
        g_next = 0x10000;
    }
}

// ---- gauges (include/nxrt/gauge.h): live handles per object type ----
namespace {
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};

size_t count_type(ObjType t) {
    std::shared_lock<os::SharedMutex> lk(g_lock);
    size_t n = 0;
    for (const auto& kv : g_handles)
        if (kv.second->type() == t) n++;
    return n;
}

GaugeHook gauge_handles("kernel.handles", [] { return static_cast<int64_t>(handle_count()); });
GaugeHook gauge_obj_thread("kernel.obj_thread", [] { return static_cast<int64_t>(count_type(ObjType::Thread)); });
GaugeHook gauge_obj_event("kernel.obj_event", [] { return static_cast<int64_t>(count_type(ObjType::Event)); });
GaugeHook gauge_obj_session("kernel.obj_session", [] { return static_cast<int64_t>(count_type(ObjType::Session)); });
GaugeHook gauge_obj_shared_mem("kernel.obj_shared_mem", [] { return static_cast<int64_t>(count_type(ObjType::SharedMemory)); });
GaugeHook gauge_obj_transfer_mem("kernel.obj_transfer_mem", [] { return static_cast<int64_t>(count_type(ObjType::TransferMemory)); });
GaugeHook gauge_obj_process("kernel.obj_process", [] { return static_cast<int64_t>(count_type(ObjType::Process)); });
GaugeHook gauge_obj_port("kernel.obj_port", [] { return static_cast<int64_t>(count_type(ObjType::Port)); });
}

}
