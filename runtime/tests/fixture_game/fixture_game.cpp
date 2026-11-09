// SPDX-License-Identifier: GPL-3.0-or-later
// The neutral fixture game the base's tests register (nxrt/game.h; cmake/tests.cmake REGISTER): an identity and file names that belong to no
// title, the base's own title defaults (no title_defaults table), no hooks, sites or probes. A base test that needs a game value asserts this
// one; tests of a real game's tables live in that game's folder (games/<id>/runtime/tests).
#include "fixture_game.h"

namespace nxtest {

const nxrt::game::Info& fixture_game() {
    static const nxrt::game::Info info = [] {
        nxrt::game::Info g;
        g.id = "fixture";
        g.title = "Fixture Game";
        g.product = "Fixture Game PC";
        g.short_title = "Fixture";
        g.program_id = kFixtureProgramId;
        g.version = "1.0.0";
        g.exe_name = "fixture.exe";
        g.setup_exe_name = "fixture_setup.exe";
        g.launcher_ini = "fixture_launcher.ini";
        g.game_ini = "fixture.ini";
        g.shader_pack = "shader_cache.fixtureshc";
        g.save_folder = "FixtureGame";
        return g;
    }();
    return info;
}

}  // namespace nxtest

void nxrt_register_fixture_game() { nxrt::game::set_current(&nxtest::fixture_game()); }
