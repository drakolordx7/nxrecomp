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

// The waits and state keys that were the base's own before the untangle (test_tt_script.cpp there).
namespace {
constexpr const char* kGuid1 = "01234567-89ab-cdef-0123-456789abcdef";
constexpr const char* kHex1 = "67452301ab89efcd0123456789abcdef";
constexpr const char* kGuid2 = "fedcba98-7654-3210-fedc-ba9876543210";
constexpr const char* kHex2 = "98badcfe54761032fedcba9876543210";
}

NXTEST(area_and_world_guids_with_timeout) {
    Script sc;
    REQUIRE(parse_ok(std::string("@area ") + kGuid1 + " timeout=90\n@world " + kGuid2 + "\n", sc));
    REQUIRE(sc.steps.size() == 2);
    CHECK_EQ(sc.steps[0].kind, StepKind::Game);
    CHECK_EQ(sc.steps[0].text, std::string("area"));
    CHECK_EQ(tt_guid_hex(sc.steps[0].id), std::string(kHex1));
    CHECK_EQ(sc.steps[0].timeout, (int64_t)90);
    CHECK_EQ(sc.steps[1].text, std::string("world"));
    CHECK_EQ(tt_guid_hex(sc.steps[1].id), std::string(kHex2));
    CHECK_EQ(sc.steps[1].timeout, (int64_t)-1);
}

NXTEST(pos_radius_and_timeout_defaults) {
    Script sc;
    REQUIRE(parse_ok("@pos 1 2 3\n@pos 1 2 3 0.5 timeout=10\n@pos 4 5 6 timeout=7\n", sc));
    REQUIRE(sc.steps.size() == 3);
    CHECK_EQ(sc.steps[0].x, 1.0);
    CHECK_EQ(sc.steps[0].y, 2.0);
    CHECK_EQ(sc.steps[0].z, 3.0);
    CHECK_EQ(sc.steps[0].r, 1.0);
    CHECK_EQ(sc.steps[0].timeout, (int64_t)-1);
    CHECK_EQ(sc.steps[1].r, 0.5);
    CHECK_EQ(sc.steps[1].timeout, (int64_t)10);
    CHECK_EQ(sc.steps[2].x, 4.0);
    CHECK_EQ(sc.steps[2].r, 1.0);
    CHECK_EQ(sc.steps[2].timeout, (int64_t)7);
}

NXTEST(igt_run_and_mash_parse) {
    Script sc;
    REQUIRE(parse_ok("@igt 12.5 timeout=60\n@igt 3\n@run 2.5 timeout=100\n@run 0 soft\n@mash A+B 5000 area 01234567-89ab-cdef-0123-456789abcdef timeout=1000\n", sc));
    REQUIRE(sc.steps.size() == 5);
    CHECK_EQ(sc.steps[0].x, 12.5);
    CHECK_EQ(sc.steps[0].timeout, (int64_t)60);
    CHECK_EQ(sc.steps[1].timeout, (int64_t)-1);
    CHECK_EQ(sc.steps[2].x, 2.5);
    CHECK_EQ(sc.steps[2].count, (int64_t)0);
    CHECK_EQ(sc.steps[3].count, (int64_t)1);          // soft
    CHECK_EQ(sc.steps[4].buttons, (1ull << 0) | (1ull << 1));
    CHECK_EQ(sc.steps[4].x, 5000.0);
    CHECK_EQ(sc.steps[4].text, std::string("area"));
    CHECK_EQ(sc.steps[4].timeout, (int64_t)1000);
    expect_err("@igt nan\n", 1, "bad number");
    expect_err("@run -1\n", 1, "must be >= 0");
    expect_err("@mash A 5000 room x\n", 1, "@mash");
    expect_err("wait 1\n@pos 1 2\n", 2, "expected");
}

NXTEST(assert_health_and_ids_are_game_fields) {
    Script sc;
    REQUIRE(parse_ok(std::string("@assert health < 50\n@assert PX >= 1\n@assert speed > 0\n@assert area == ") + kGuid1 + "\n@assert world != " + kGuid2 + "\n", sc));
    REQUIRE(sc.steps.size() == 5);
    CHECK_EQ(sc.steps[0].field, AssertField::Game);
    CHECK_EQ(sc.steps[0].name2, std::string("health"));
    CHECK_EQ(sc.steps[1].name2, std::string("px"));
    CHECK_EQ(sc.steps[2].name2, std::string("speed"));
    CHECK_EQ(sc.steps[3].field, AssertField::GameId);
    CHECK_EQ(sc.steps[3].op, AssertOp::Eq);
    CHECK_EQ(tt_guid_hex(sc.steps[3].id), std::string(kHex1));
    CHECK_EQ(sc.steps[4].op, AssertOp::Ne);
    CHECK_EQ(tt_guid_hex(sc.steps[4].id), std::string(kHex2));
    expect_err(std::string("wait 1\n@assert area < ") + kGuid1 + "\n", 2, "only == and !=");
}

NXTEST(id_errors_name_the_line) {
    RegionTable rt;
    std::string e;
    CHECK_MSG(rt.load_text(region_text(), &e), e);
    expect_err("wait 1\n@area 01234567-89ab-cdef-0123-45678\n", 2, "not a GUID");
    expect_err("wait 1\n\n@world nosuch\n", 3, "nosuch", &rt);
    expect_err("wait 1\n@area frigate\n", 2, "has no area", &rt);
    expect_err(std::string("wait 1\n\n@area ") + kGuid1 + " timeout=\n", 3, "bad number");
    expect_err(std::string("@area ") + kGuid1 + " timeout=1 timeout=2\n", 1, "timeout given twice");
}
