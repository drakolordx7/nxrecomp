// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the base's runtime/tests (test_svc_am.cpp) in the untangle: these cases check Metroid Prime Remastered's tables (title defaults,
// play-mode vocabulary and factory settings, test-tooling probes), so they run with the mpr module registered.
// appletOE / appletAE (S3c): command replies, the applet message queue, launch parameters, storages, save data, the language.
// Request bytes and expected reply bytes are written out from Switchbrew "Applet Manager services" and ScarletNX am.swipc, and from
// Ryujinx for the result codes (module 128: result = module | description << 9), not read from the service.
#include <filesystem>
#include <windows.h>
#include <cstring>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/services_api.h"
#include "svc_test_util.h"

NXTEST_GROUP(mpr_svc_am)

using namespace nxrt;
namespace amctl = nxrt::services::amctl;

namespace {

// OpenApplicationProxy(pid) with a copy of a process handle. The service closes the copy, so the test hands it an event handle.
nxsvc::Session open_proxy() {
    nxsvc::Session oe = nxsvc::connect("appletOE");
    if (!oe.valid()) return nxsvc::Session();
    services::ServiceEvent process_stand_in;
    nxsvc::Reply r = oe.call(0, nxsvc::Args().with_pid().hcopy(process_stand_in.new_handle()));
    if (!r.ok() || r.move.empty()) return nxsvc::Session();
    return oe.sub(r);
}

bool signalled(uint32_t h) {
    int32_t idx = -1;
    return kernel::wait_synchronization(&h, 1, 0, &idx) == 0;
}

// The fixed launch parameter layout bytes, from the Switchbrew / Ryujinx layout: magic 0xC79497CA, flag 1, Uid low word, Uid high word.
// The test profile's user id (config below) is hi = 1, lo = 2.
void set_test_user() {
    config_set("account", "uid", "00000000000000010000000000000002");
    services::profile_reload();
}

void clear_config() {
    config_clear();
    services::profile_reload();
}

}

NXTEST(am_application_functions_fixed_replies) {
    nxsvc::Env env;
    clear_config();
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session app = proxy.sub(proxy.call(20));
    REQUIRE(app.valid());

    // GetDisplayVersion: "1.0.0", NUL-terminated, 16 bytes.
    nxsvc::Reply dv = app.call(23);
    CHECK_EQ(dv.result, 0u);
    REQUIRE(dv.out.size() >= 16);
    const uint8_t want_dv[16] = {'1', '.', '0', '.', '0', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(std::memcmp(dv.out.data(), want_dv, 16) == 0);

    CHECK_EQ(app.call(40).get<uint8_t>(0), 1u);

    // GetPseudoDeviceId: the fixed 16 bytes "NXRT-DEVICE-0001".
    nxsvc::Reply pd = app.call(50);
    CHECK_EQ(pd.result, 0u);
    REQUIRE(pd.out.size() >= 16);
    CHECK(std::memcmp(pd.out.data(), "NXRT-DEVICE-0001", 16) == 0);

    // GetPreviousProgramIndex: -1, launched by the system.
    CHECK_EQ(app.call(123).get<int32_t>(0), -1);

    // The events that never fire hand out a handle that is not signalled.
    nxsvc::Reply ne = app.call(150);
    REQUIRE(ne.copy.size() == 1);
    CHECK(!signalled(ne.copy[0]));
    kernel::handle_close(ne.copy[0]);

    // TryPop on the notification channel: no data, 128-2 = 0x480.
    CHECK_EQ(app.call(151).result, 0x480u);
    CHECK_EQ(app.call(141).result, 0x480u);

    CHECK_EQ(app.call(22, nxsvc::Args().u32(0x2A2)).result, 0u);
    CHECK_EQ(app.call(33).result, 0u);
    CHECK_EQ(app.call(90, nxsvc::Args().u8(1)).result, 0u);
}

NXTEST(am_ensure_save_data) {
    nxsvc::Env env;
    const std::string dir = (std::filesystem::temp_directory_path() / ("nxrt_am_save_" + std::to_string(GetCurrentProcessId()))).string();
    std::filesystem::remove_all(dir);
    config_set("fs", "save_dir", dir.c_str());
    nxsvc::Session proxy = open_proxy();
    REQUIRE(proxy.valid());
    nxsvc::Session app = proxy.sub(proxy.call(20));
    REQUIRE(app.valid());
    int64_t size = 0, journal = 0;
    CHECK(services::fsapi::account_save_size(1, 2, &size, &journal) != 0);          // nothing there yet
    // EnsureSaveData(Uid: first u64 = hi 1, second = lo 2): creates the account save data, nothing missing.
    nxsvc::Reply r = app.call(20, nxsvc::Args().u64(1).u64(2));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint64_t>(0), 0ull);
    CHECK_EQ(services::fsapi::account_save_size(1, 2, &size, &journal), 0u);
    CHECK_EQ(size, 4ll << 20);
    // GetSaveDataSize(type 1 = account, Uid) -> normal, journal
    r = app.call(26, nxsvc::Args().u8(1).align(8).u64(1).u64(2));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int64_t>(0), 4ll << 20);
    CHECK_EQ(r.get<int64_t>(8), 1ll << 20);
    std::filesystem::remove_all(dir);
    clear_config();
}

