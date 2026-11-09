// Where the optional local test data lives. Two roots, each from the environment first, then from the build (cmake/tests.cmake):
//   nx_root()     the NX folder (NX_ROOT; build: the nearest parent of the base checkout with a games/ folder): refs/ ...
//   project_root() the folder of the game whose data the data-driven tests read (NXRT_TEST_DATA_GAME_DIR; build: the cache variable of
//                 the same name, empty by default): work/shader_cache, work/nvn_refs, game/romfs ...
// An empty root leaves the paths relative, so the tests that need the data skip.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

#ifndef NXRT_NX_ROOT
#define NXRT_NX_ROOT ""
#endif
#ifndef NXRT_TEST_DATA_GAME_DIR
#define NXRT_TEST_DATA_GAME_DIR ""
#endif

namespace nxtest {

inline std::filesystem::path root_from(const char* env, const char* built) {
    if (const char* e = std::getenv(env); e && *e) return std::filesystem::path(e);
    return std::filesystem::path(built);
}

inline const std::filesystem::path& nx_root() {
    static const std::filesystem::path root = root_from("NX_ROOT", NXRT_NX_ROOT);
    return root;
}

inline const std::filesystem::path& project_root() {
    static const std::filesystem::path root = root_from("NXRT_TEST_DATA_GAME_DIR", NXRT_TEST_DATA_GAME_DIR);
    return root;
}

// `rel` below the game data folder, with '/' separators (a trailing '/' is kept).
inline std::string project_path(const std::string& rel) {
    return (project_root() / rel).generic_string();
}
// `rel` below the NX folder.
inline std::string nx_path(const std::string& rel) {
    return (nx_root() / rel).generic_string();
}

}  // namespace nxtest
