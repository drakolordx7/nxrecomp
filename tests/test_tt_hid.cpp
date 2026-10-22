// SPDX-License-Identifier: GPL-3.0-or-later
// The hid adapter of the test tooling (tt_hid.cpp): only built when S3c's input_source.h is in the tree (the `services` branch).
#include "test_framework.h"

#if __has_include("nxrt/input_source.h")
#include <memory>

#include "nxrt/config.h"
#include "nxrt/input_source.h"
#include "nxrt/tt.h"

NXTEST_GROUP(tt_hid)

NXTEST(replayed_pad_reaches_the_hid_source) {
    using namespace nxrt;
    // a live source with player 1 connected and the stick pushed: replay must override player 1 and keep the rest
    auto live = std::make_shared<hid::ManualInputSource>();
    live->connect(0, hid::NpadStyle::FullKey);
    live->press(0, hid::btn(hid::kBtnZL));
    hid::set_input_source(live);

    tt::InputTrace tr;
    tt::PadFrame p;
    tr.record(0, p);
    p.buttons = hid::btn(hid::kBtnA) | hid::btn(hid::kBtnUp);
    p.ly = 20000;
    tr.record(3, p);
    tr.end_frame = 10;
    std::string err;
    REQUIRE(tr.write("tt_hid_test.nxin", &err));

    nxrt::config_clear();
    tt::reset_for_tests();
    nxrt::config_set("test", "replay", "tt_hid_test.nxin");
    nxrt::config_set("test", "frame_source", "manual");
    nxrt::config_set("test", "out", "tt_test_out/hid");
    REQUIRE(tt::init());
    for (int i = 0; i < 5; i++) {
        tt::frame_boundary(nullptr);
        hid::InputSnapshot s;
        auto src = hid::input_source();
        REQUIRE(src != nullptr);
        src->poll(s);
        if (i < 3) CHECK_EQ(s.npad[0].buttons, 0ull);
        else {
            CHECK_EQ(s.npad[0].buttons, hid::btn(hid::kBtnA) | hid::btn(hid::kBtnUp));
            CHECK_EQ(s.npad[0].left.y, 20000);
        }
    }
    tt::shutdown();
    std::remove("tt_hid_test.nxin");
    // the live source is what the recorder samples when nothing is replayed
    hid::set_input_source(live);
    tt::reset_for_tests();
    nxrt::config_clear();
    nxrt::config_set("test", "record", "tt_hid_rec.nxin");
    nxrt::config_set("test", "frame_source", "manual");
    nxrt::config_set("test", "out", "tt_test_out/hid2");
    REQUIRE(tt::init());
    for (int i = 0; i < 3; i++) tt::frame_boundary(nullptr);
    tt::shutdown();
    tt::InputTrace rec;
    REQUIRE(rec.read("tt_hid_rec.nxin", &err));
    CHECK_EQ(rec.at(1).buttons, hid::btn(hid::kBtnZL));
    std::remove("tt_hid_rec.nxin");
    hid::set_input_source(nullptr);
}
#else
NXTEST_GROUP(tt_hid)
NXTEST(skipped_without_input_source_header) { nxtest::skip("nxrt/input_source.h (S3c, services branch) is not in this tree"); }
#endif
