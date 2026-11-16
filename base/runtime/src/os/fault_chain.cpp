// SPDX-License-Identifier: GPL-3.0-or-later
// The handler chain of nxrt/os/fault.h. A registration builds a new immutable snapshot and publishes it with one atomic store; the dispatcher
// (a signal handler on Linux) only loads the pointer, so it needs no lock. Old snapshots are kept (a few hundred bytes each, registrations
// are rare), because a handler running on another thread may still be walking one.
#include "fault_chain.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace nxrt::os {
namespace {

constexpr int kMaxHandlers = 16;

struct Entry {
    FaultHandler fn;
    void* user;
    int priority;
    int id;
    uint32_t seq;
};
struct Chain {
    int n = 0;
    Entry e[kMaxHandlers];
};

std::mutex g_mu;
std::once_flag g_once;
std::atomic<const Chain*> g_chain{nullptr};
std::atomic<FaultHandler> g_unhandled{nullptr};
std::atomic<void*> g_unhandled_user{nullptr};
int g_next_id = 1;
uint32_t g_next_seq = 0;

}  // namespace

namespace detail {

bool chain_run(const FaultInfo& fi) {
    const Chain* c = g_chain.load(std::memory_order_acquire);
    if (!c) return false;
    for (int i = 0; i < c->n; ++i)
        if (c->e[i].fn(fi, c->e[i].user) == FaultResult::Handled) return true;
    return false;
}

bool unhandled_run(const FaultInfo& fi) {
    FaultHandler h = g_unhandled.load(std::memory_order_acquire);
    return h && h(fi, g_unhandled_user.load(std::memory_order_acquire)) == FaultResult::Handled;
}

}  // namespace detail

int fault_register(FaultHandler h, void* user, int priority) {
    if (!h) return -1;
    std::call_once(g_once, [] { detail::platform_install(); });
    fault_thread_init();
    std::lock_guard<std::mutex> lk(g_mu);
    const Chain* old = g_chain.load(std::memory_order_relaxed);
    if (old && old->n >= kMaxHandlers) return -1;
    Chain* c = new Chain;
    if (old) *c = *old;
    const int id = g_next_id++;
    c->e[c->n++] = Entry{h, user, priority, id, g_next_seq++};
    std::stable_sort(c->e, c->e + c->n, [](const Entry& a, const Entry& b) { return a.priority != b.priority ? a.priority > b.priority : a.seq < b.seq; });
    g_chain.store(c, std::memory_order_release);
    return id;
}

void fault_unregister(int id) {
    std::lock_guard<std::mutex> lk(g_mu);
    const Chain* old = g_chain.load(std::memory_order_relaxed);
    if (!old) return;
    Chain* c = new Chain;
    for (int i = 0; i < old->n; ++i)
        if (old->e[i].id != id) c->e[c->n++] = old->e[i];
    g_chain.store(c, std::memory_order_release);
}

void fault_set_unhandled(FaultHandler h, void* user) {
    std::call_once(g_once, [] { detail::platform_install(); });
    g_unhandled_user.store(user, std::memory_order_release);
    g_unhandled.store(h, std::memory_order_release);
    if (h) detail::platform_unhandled_set();
}

}  // namespace nxrt::os
