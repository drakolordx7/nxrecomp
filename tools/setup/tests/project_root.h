// Where the optional real-data inputs of the tests live: the game folder this setup tool is built for (NXS_TEST_GAME_DIR from CMake,
// env NX_GAME_DIR overrides it; a worktree of the game, which has no game/ folder, reads games/<id> of the NX folder) holds game/keys
// and game/exefs, and the base's Python (NXS_TEST_PYTHON from CMake, env NX_PYTHON
// overrides it) runs the pack script. A test whose input is absent skips.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace nxs_test {

inline std::filesystem::path game_dir() {
    if (const char* e = std::getenv("NX_GAME_DIR"); e && *e) return std::filesystem::path(e);
    std::error_code ec;
    const std::filesystem::path dir(NXS_TEST_GAME_DIR);
    if (std::filesystem::is_directory(dir / "game", ec)) return dir;
    const std::filesystem::path main_checkout = std::filesystem::path(NXS_TEST_NX_ROOT) / "games" / NXS_TEST_GAME_ID;
    return std::filesystem::is_directory(main_checkout / "game", ec) ? main_checkout : dir;
}

// `rel` (written with '/') below <game dir>/game, with the platform's separators.
inline std::string game_data_path(const std::string& rel) {
    return (game_dir() / "game" / rel).make_preferred().string();
}

inline std::filesystem::path python_exe() {
    if (const char* e = std::getenv("NX_PYTHON"); e && *e) return std::filesystem::path(e).make_preferred();
    return std::filesystem::path(NXS_TEST_PYTHON).make_preferred();
}

// The first build id of the game's manifest: the build id of the main module of the dump in game/exefs.
inline const char* first_build_id() { return NXS_TEST_BUILD_ID; }

}  // namespace nxs_test
