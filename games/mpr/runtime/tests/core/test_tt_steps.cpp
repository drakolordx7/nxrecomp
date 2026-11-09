// SPDX-License-Identifier: GPL-3.0-or-later
// Moved from the base's runtime/tests (test_tt_script.cpp) in the untangle: the script steps Metroid Prime Remastered registers with the test
// tooling (mpr_tt_steps.cpp): their syntax, item names, error lines. They parse only with the mpr module registered.
#include "test_framework.h"

#include <cstdint>
#include <string>

#include "nxrt/tt_script.h"

NXTEST_GROUP(mpr_tt_steps)

using namespace nxrt::tt;

namespace {

// Region table fixture: two worlds, each with one area. "frigate" and "ruins" are world-only entries.
constexpr const char* kWorld1 = "10000000-0000-0000-0000-000000000001";
constexpr const char* kArea1 = "10000000-0000-0000-0000-0000000000a1";
constexpr const char* kWorld2 = "20000000-0000-0000-0000-000000000002";
constexpr const char* kArea2 = "20000000-0000-0000-0000-0000000000b2";

std::string region_text() {
    return std::string("# name<TAB>world<TAB>area\n") + "frigate\t" + kWorld1 + "\t\n" +
           "frigate_cargo_freight_lift\t" + kWorld1 + "\t" + kArea1 + "\n" + "\n" + "ruins\t" + kWorld2 + "\t\r\n" +
           "ruins_hall\t" + kWorld2 + "\t" + kArea2 + "\r\n";
}

bool parse_ok(const std::string& text, Script& sc, const RegionTable* rt = nullptr) {
    std::string err;
    if (parse_script(text, "t.txt", rt, &sc, &err)) return true;
    ::nxtest::fail(__FILE__, __LINE__, "parse_script(text) succeeds", err);
    return false;
}

void expect_err(const std::string& text, int line, const std::string& needle, const RegionTable* rt = nullptr) {
    Script sc;
    std::string err;
    bool ok = parse_script(text, "t.txt", rt, &sc, &err);
    CHECK_MSG(!ok, "parse unexpectedly succeeded: " + text);
    const std::string prefix = "t.txt:" + std::to_string(line) + ": ";
    CHECK_MSG(err.rfind(prefix, 0) == 0, err);
    CHECK_MSG(err.find(needle) != std::string::npos, err);
}

}

NXTEST(warp_resolves_region_names) {
    RegionTable rt;
    std::string e;
    CHECK_MSG(rt.load_text(region_text(), &e), e);

    Script sc;
    REQUIRE(parse_ok(std::string("@warp frigate frigate_cargo_freight_lift\n@warp RUINS ruins_hall\n") + "@warp " +
                         kWorld1 + " " + kArea2 + "\n",
                     sc, &rt));
    REQUIRE(sc.steps.size() == 3);
    CHECK_EQ(sc.steps[0].kind, StepKind::Game);
    CHECK_EQ(sc.steps[0].name1, std::string("frigate"));
    CHECK_EQ(sc.steps[0].name2, std::string("frigate_cargo_freight_lift"));
    CHECK_EQ(tt_guid_to_text(sc.steps[0].id), std::string(kArea1));
    CHECK_EQ(tt_guid_to_text(sc.steps[0].id2), std::string(kWorld1));
    CHECK_EQ(tt_guid_to_text(sc.steps[1].id), std::string(kArea2));
    CHECK_EQ(tt_guid_to_text(sc.steps[1].id2), std::string(kWorld2));
    CHECK_EQ(tt_guid_to_text(sc.steps[2].id), std::string(kArea2));
    CHECK_EQ(tt_guid_to_text(sc.steps[2].id2), std::string(kWorld1));
}

NXTEST(grant_names_numbers_and_all) {
    Script sc;
    REQUIRE(parse_ok("@grant MorphBall\n@grant missiles 250 250\n@grant Charge_Beam\n@grant 0x12 1\n@grant all\n", sc));
    REQUIRE(sc.steps.size() == 5);
    CHECK_EQ(sc.steps[0].kind, StepKind::Game);
    CHECK_EQ(sc.steps[0].count, (int64_t)0x10);
    CHECK_EQ(sc.steps[0].x, 1.0);
    CHECK_EQ(sc.steps[1].count, (int64_t)4);
    CHECK_EQ(sc.steps[1].x, 250.0);
    CHECK_EQ(sc.steps[1].y, 250.0);
    CHECK_EQ(sc.steps[2].count, (int64_t)0xa);
    CHECK_EQ(sc.steps[3].count, (int64_t)0x12);
    CHECK_EQ(sc.steps[4].count, (int64_t)-1);
    expect_err("@grant sword\n", 1, "unknown item");
}

NXTEST(call_parses_offset_and_argument_text) {
    Script sc;
    REQUIRE(parse_ok("@call 0xb03a58 $mgr\n@call 0xc88f30 $ps 4 250\n", sc));
    REQUIRE(sc.steps.size() == 2);
    CHECK_EQ(sc.steps[0].kind, StepKind::Game);
    CHECK_EQ(sc.steps[0].count, (int64_t)0xb03a58);
    CHECK_EQ(sc.steps[0].text, std::string("$mgr"));
    CHECK_EQ(sc.steps[1].text, std::string("$ps 4 250"));
    expect_err("@call\n", 1, "@call <offset>");
}

NXTEST(watch_on_defaults_to_both_axes) {
    Script sc;
    REQUIRE(parse_ok("@watch on\n", sc));
    REQUIRE(sc.steps.size() == 1);
    CHECK_EQ(sc.steps[0].kind, StepKind::Game);
    CHECK_EQ(sc.steps[0].text, std::string("on"));
    CHECK_EQ(sc.steps[0].y, 3.0);
}

NXTEST(watch_on_selects_vely_or_posy) {
    Script sc;
    REQUIRE(parse_ok("@watch on vely\n@watch on posy\n@watch on both\n", sc));
    REQUIRE(sc.steps.size() == 3);
    CHECK_EQ(sc.steps[0].kind, StepKind::Game);
    CHECK_EQ(sc.steps[0].y, 1.0);
    CHECK_EQ(sc.steps[1].y, 2.0);
    CHECK_EQ(sc.steps[2].y, 3.0);
}

NXTEST(watch_off_parses) {
    Script sc;
    REQUIRE(parse_ok("@watch off\n", sc));
    REQUIRE(sc.steps.size() == 1);
    CHECK_EQ(sc.steps[0].kind, StepKind::Game);
    CHECK_EQ(sc.steps[0].text, std::string("off"));
}

NXTEST(watch_errors) {
    expect_err("@watch\n", 1, "@watch on [vely|posy|both]");
    expect_err("@watch maybe\n", 1, "expected on or off");
    expect_err("@watch on bogus\n", 1, "expected vely, posy or both");
}

NXTEST(rays_parses_in_and_out_files) {
    Script sc;
    REQUIRE(parse_ok("@rays a.txt b.txt\n", sc));
    REQUIRE(sc.steps.size() == 1);
    CHECK_EQ(sc.steps[0].kind, StepKind::Game);
    CHECK_EQ(sc.steps[0].text, std::string("a.txt b.txt"));
}

NXTEST(rays_wrong_argument_count_is_usage_error) {
    expect_err("@rays\n", 1, "@rays <in-file> <out-file>");
    expect_err("@rays a.txt\n", 1, "@rays <in-file> <out-file>");
    expect_err("@rays a b c\n", 1, "@rays <in-file> <out-file>");
}

NXTEST(objs_parses_class_list) {
    Script sc;
    REQUIRE(parse_ok("@objs *\n", sc));
    REQUIRE(sc.steps.size() == 1);
    CHECK_EQ(sc.steps[0].kind, StepKind::Game);
    CHECK_EQ(sc.steps[0].text, std::string("*"));
}

NXTEST(probe_parses_in_and_out_files) {
    Script sc;
    REQUIRE(parse_ok("@probe a.txt b.txt\n", sc));
    REQUIRE(sc.steps.size() == 1);
    CHECK_EQ(sc.steps[0].kind, StepKind::Game);
    CHECK_EQ(sc.steps[0].text, std::string("a.txt b.txt"));
}

NXTEST(error_lines_of_the_world_steps) {
    RegionTable rt;
    std::string e;
    CHECK_MSG(rt.load_text(region_text(), &e), e);
    expect_err("wait 1\n\n@warp frigate\n", 3, "expected");
    expect_err("@warp 0123 0123\n", 1, "not a GUID");
    expect_err("@warp frigate nosuch\n", 1, "nosuch", &rt);
    expect_err("@warp frigate ruins\n", 1, "has no area", &rt);
    expect_err("@probe a.txt\n", 1, "@probe <in-file> <out-file>");
}

NXTEST(give_and_grant_share_the_item_names) {
    Script sc;
    REQUIRE(parse_ok("@give MorphBall 1\n@give charge_beam 1\n@give 4 50\n@grant MorphBall\n@grant Charge_Beam\n@grant 4 250 250\n@grant bombs\n", sc));
    REQUIRE(sc.steps.size() == 7);
    CHECK_EQ(sc.steps[0].kind, StepKind::Game);
    CHECK_EQ(sc.steps[0].x, 16.0);          // MorphBall
    CHECK_EQ(sc.steps[1].x, 10.0);          // ChargeBeam
    CHECK_EQ(sc.steps[2].x, 4.0);
    CHECK_EQ(sc.steps[2].y, 50.0);
    CHECK_EQ(sc.steps[3].count, (int64_t)16);
    CHECK_EQ(sc.steps[4].count, (int64_t)10);
    CHECK_EQ(sc.steps[5].count, (int64_t)4);
    CHECK_EQ(sc.steps[6].count, (int64_t)6);
    expect_err("@give Nonsense 1\n", 1, "unknown item");
    expect_err("@give MorphBall\n", 1, "@give <item> <amount>");
}

NXTEST(state_keys_of_the_game_are_assert_fields) {
    Script sc;
    REQUIRE(parse_ok("@assert state == 2\n@assert MSL >= 5\n@mashuntil START every 5 until state >= 0\n", sc));
    REQUIRE(sc.steps.size() == 3);
    CHECK_EQ(sc.steps[0].field, AssertField::Game);
    CHECK_EQ(sc.steps[0].name2, std::string("state"));
    CHECK_EQ(sc.steps[1].name2, std::string("msl"));
    CHECK_EQ(sc.steps[1].op, AssertOp::Ge);
    CHECK_EQ(sc.steps[2].kind, StepKind::PressUntil);
    CHECK_EQ(sc.steps[2].name2, std::string("state"));
    expect_err("@assert bogus == 1\n", 1, "unknown assert field 'bogus'");
}
