// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the base's runtime/tests (test_svc_common.cpp) in the untangle: these cases check Metroid Prime Remastered's tables (title defaults,
// play-mode vocabulary and factory settings, test-tooling probes), so they run with the mpr module registered.
// Service library framework: registration, events, shared memory, the title profile, the test client.
#include "svc_test_util.h"

#include <algorithm>

#include "nxrt/config.h"

NXTEST_GROUP(mpr_svc_common)

using namespace nxrt;

NXTEST(common_profile_defaults) {
    nxsvc::Env env;
    config_clear();
    services::profile_reload();
    const services::TitleProfile& p = services::profile();
    CHECK_EQ(p.program_id, 0x010012101468C000ull);
    CHECK_EQ(p.account_save_size, 4ull << 20);
    CHECK_EQ(p.account_save_journal, 1ull << 20);
    CHECK_EQ(static_cast<uint32_t>(p.language), static_cast<uint32_t>(services::Language::AmericanEnglish));
    CHECK(p.docked);
    // 13 supported languages (control.nacp)
    CHECK_EQ(__builtin_popcount(p.supported_languages), 13);
    CHECK_EQ(services::language_code_u64(services::Language::AmericanEnglish), 0x53552d6e65ull);   // "en-US"
}

NXTEST(common_profile_overrides) {
    nxsvc::Env env;
    config_clear();
    config_set("system", "language", "Japanese");
    config_set("system", "region", "Europe");
    config_set("am", "operation_mode", "handheld");
    config_set("account", "uid", "00000000000000010000000000000002");
    services::profile_reload();
    const services::TitleProfile& p = services::profile();
    CHECK_EQ(static_cast<uint32_t>(p.language), static_cast<uint32_t>(services::Language::Japanese));
    CHECK_EQ(p.region, 2);
    CHECK(!p.docked);
    CHECK_EQ(p.user_uid_hi, 1ull);
    CHECK_EQ(p.user_uid_lo, 2ull);
    config_clear();
    services::profile_reload();
}

NXTEST(common_program_id_default) {
    config_clear();
    CHECK_EQ(services::program_id_now(), 0x010012101468C000ull);
    config_set("process", "program_id", "0x1234");
    CHECK_EQ(services::program_id_now(), 0x1234ull);
    config_clear();
}

