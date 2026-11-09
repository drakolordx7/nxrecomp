// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the base's runtime/tests (test_svc_misc.cpp) in the untangle: these cases check Metroid Prime Remastered's tables (title defaults,
// play-mode vocabulary and factory settings, test-tooling probes), so they run with the mpr module registered.
// Small services of the title's Service Access Control list (S3c): csrng, ldr:ro, mm:u, hwopus, aoc:u, caps:u, mii:u, irs,
// nfc:user, nfp:user, ectx:aw, banana, htc, htcs. Request bytes and expected results are written out from Switchbrew (command tables
// and the Error_codes page), not read from the implementation.
#include "svc_test_util.h"

#include "nxrt/config.h"

NXTEST_GROUP(mpr_svc_misc)

using namespace nxrt;

// Switchbrew Error_codes: ro module 22, description 1028 (NotLoaded) = (1028 << 9) | 22.
static constexpr uint32_t kRoNotLoaded = 0x80816u;
// Switchbrew Error_codes: Sf_Hipc module 11, description 1 (NotSupported) = (1 << 9) | 11.
static constexpr uint32_t kNotSupported = 0x20Bu;

NXTEST(misc_aoc_base_id_and_empty_list) {
    nxsvc::Env env;
    config_clear();
    services::profile_reload();
    nxsvc::Session s = nxsvc::connect("aoc:u");
    REQUIRE(s.valid());
    // GetAddOnContentBaseId (cmd 5): PID-descriptor -> u64. The title's add-on base id (control.nacp) is 010012101468D000.
    nxsvc::Reply base = s.call(5, nxsvc::Args().with_pid());
    REQUIRE(base.ok());
    CHECK_EQ(base.get<uint64_t>(0), 0x010012101468D000ull);
    // CountAddOnContent (cmd 2): PID-descriptor -> s32 count = 0.
    nxsvc::Reply cnt = s.call(2, nxsvc::Args().with_pid());
    REQUIRE(cnt.ok());
    CHECK_EQ(cnt.get<int32_t>(0), 0);
    // GetAddOnContentListChangedEvent (cmd 8) returns an event handle.
    nxsvc::Reply ev = s.call(8);
    CHECK(ev.ok());
    CHECK_EQ(ev.copy.size(), 1u);
    config_clear();
    services::profile_reload();
}

