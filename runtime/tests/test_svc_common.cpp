// SPDX-License-Identifier: GPL-3.0-or-later
// Service library framework: registration, events, shared memory, the title profile, the test client.
#include "svc_test_util.h"

#include <algorithm>

#include "nxrt/config.h"

NXTEST_GROUP(svc_common)

using namespace nxrt;

NXTEST(common_connect_through_sm) {
    nxsvc::Env env;
    // A service nobody implements is still reachable as a stub (the old behaviour), a registered one is not a stub.
    nxsvc::Session s = nxsvc::connect("no:such");
    CHECK(s.valid());
    nxsvc::Reply r = s.call(0);
    CHECK_EQ(r.result, kernel::kStubService);
}

NXTEST(common_event_wait) {
    nxsvc::Env env;
    services::ServiceEvent ev;
    const uint32_t h = ev.new_handle();
    REQUIRE(h != 0);
    int32_t idx = -1;
    CHECK_EQ(kernel::wait_synchronization(&h, 1, 0, &idx), kernel::kTimedOut);
    ev.signal();
    CHECK_EQ(kernel::wait_synchronization(&h, 1, 0, &idx), 0u);
    CHECK_EQ(idx, 0);
    ev.clear();
    CHECK_EQ(kernel::wait_synchronization(&h, 1, 0, &idx), kernel::kTimedOut);
    // A second handle sees the same state.
    const uint32_t h2 = ev.new_handle();
    ev.signal();
    CHECK_EQ(kernel::wait_synchronization(&h2, 1, 0, &idx), 0u);
    kernel::handle_close(h);
    kernel::handle_close(h2);
}

NXTEST(common_shared_memory_map) {
    nxsvc::Env env;
    services::ServiceSharedMemory sm(0x3000, "test");
    REQUIRE(sm.ok());
    CHECK_EQ(sm.size(), 0x3000u);
    std::memset(reinterpret_cast<void*>(sm.host()), 0xAB, 16);
    const uint32_t h = sm.new_handle();
    REQUIRE(h != 0);
    auto obj = kernel::handle_as<kernel::KSharedMemory>(h);
    REQUIRE(obj != nullptr);
    CHECK_EQ(obj->size, 0x3000u);
    CHECK_EQ(reinterpret_cast<uint8_t*>(sm.host())[3], 0xAB);
    kernel::handle_close(h);
}

