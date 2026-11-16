// SPDX-License-Identifier: GPL-3.0-or-later
// Display HDR query (extras/display_hdr.h) on this machine: the call does not crash and reports something, and the numbers are in range
// when it is valid. Does not assert hdr_enabled either way, since the monitor's setting decides it. The full result is printed so the run
// output shows what the query found. Skips where SDL cannot open its video subsystem (no display).
#include <cstdio>

#include "test_framework.h"

#include "extras/display_hdr.h"
#include "nxrt/platform.h"

NXTEST_GROUP(display_hdr)

namespace {

namespace extras = nxrt::extras;
namespace platform = nxrt::platform;

void print_info(const extras::DisplayHdrInfo& i) {
    std::printf("display_hdr: valid=%d monitor=%s hdr_enabled=%d max_nits=%.1f sdr_white_nits=%.1f\ndisplay_hdr: how=%s\n", i.valid ? 1 : 0, i.monitor,
                i.hdr_enabled ? 1 : 0, i.max_nits, i.sdr_white_nits, i.how);
}

}  // namespace

NXTEST(query_without_platform_or_window_does_not_crash_and_reports_text) {
    extras::DisplayHdrInfo i;
    const bool ret = extras::query_display_hdr(&i);
    print_info(i);
    CHECK_EQ(ret, i.valid);
    CHECK_MSG(i.monitor[0] != '\0' || i.how[0] != '\0', "neither monitor nor how was filled");
    CHECK(!extras::query_display_hdr(nullptr));
}

NXTEST(primary_display_answers) {
    if (!platform::init()) {
        ::nxtest::skip("SDL video is not available here");
        return;
    }
    struct Shutdown {
        ~Shutdown() { platform::shutdown(); }
    } shutdown;
    extras::DisplayHdrInfo i;
    const bool ret = extras::query_display_hdr(&i);
    print_info(i);
    CHECK_EQ(ret, i.valid);
    if (!i.valid) {
        ::nxtest::skip("SDL reports no display on this machine (see the how line above)");
        return;
    }
    CHECK(i.max_nits >= 0.0f);
    CHECK(i.sdr_white_nits >= 80.0f && i.sdr_white_nits <= 1000.0f);
    CHECK(!i.hdr_enabled || i.max_nits >= i.sdr_white_nits);
}
