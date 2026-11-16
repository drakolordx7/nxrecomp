// SPDX-License-Identifier: GPL-3.0-or-later
// Launcher helpers in apps/launcher/game_info.cpp: game folder checks, the settings file, command-line quoting.
// Only temp folders are touched; no registry, no real launcher folder, no game process is started.
#include "test_framework.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "game_info.h"
#include "nxrt/os/process.h"

NXTEST_GROUP(launcher_game_info)

namespace {

namespace fs = std::filesystem;
using nxl::GameStatus;
using nxl::LauncherConfig;

void write_file(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<unsigned> counter{0};
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec);
        path = base / ("nxl_game_info_test_" + std::to_string(nxrt::os::process::pid()) + "_" +
                       std::to_string(counter++));
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const {
        const auto u = path.u8string();
        return std::string(reinterpret_cast<const char*>(u.c_str()), u.size());
    }
};

// The setup tool's state file header and a finished run's meta lines (pipeline.cpp, State::save and run_setup).
const char* kFinishedState =
    "mprs_setup_state\t1\n"
    "meta\tbc7_quality\t3\n"
    "meta\tfinished\t1\n";

}  // namespace

// ---- split_args (the text of --game-args and --setup-args) -------------------------------------------------------------

using Args = std::vector<std::string>;

NXTEST(split_args_plain_words) {
    CHECK((nxl::split_args("--a --b  c\td") == Args{"--a", "--b", "c", "d"}));
    CHECK(nxl::split_args("").empty());
    CHECK(nxl::split_args("   \t ").empty());
}

NXTEST(split_args_quotes_group_words) {
    CHECK((nxl::split_args("--dir \"C:\\My Games\\x\" --b") == Args{"--dir", "C:\\My Games\\x", "--b"}));
    CHECK((nxl::split_args("a\"b c\"d") == Args{"ab cd"}));
    CHECK((nxl::split_args("\"\" x") == Args{"", "x"}));
}

NXTEST(split_args_backslash_rules_of_a_windows_command_line) {
    // a backslash before a quote: pairs give a backslash each, an odd one escapes the quote
    CHECK((nxl::split_args("\"say \\\"hi\\\"\"") == Args{"say \"hi\""}));
    CHECK((nxl::split_args("\"C:\\dir\\\\\" next") == Args{"C:\\dir\\", "next"}));
    // backslashes elsewhere stay
    CHECK((nxl::split_args("C:\\a\\b") == Args{"C:\\a\\b"}));
}

NXTEST(split_args_non_ascii) {
    CHECK((nxl::split_args("\"Caf\xC3\xA9 \xE4\xB8\xAD\xE6\x96\x87\"") == Args{"Caf\xC3\xA9 \xE4\xB8\xAD\xE6\x96\x87"}));
}

// ---- launcher config text --------------------------------------------------------------------------------------------

NXTEST(launcher_config_round_trip_non_ascii) {
    LauncherConfig in;
    in.game_dir = "D:\\Games\\Caf\u00e9 \u4e2d\u6587\\Some Game";
    const std::string text = nxl::format_launcher_config(in);
    CHECK(text.find("\xC3\xA9") != std::string::npos);   // e acute, as UTF-8 bytes

    LauncherConfig out;
    out.game_dir = "unchanged";
    CHECK(nxl::parse_launcher_config(text, &out));
    CHECK(out.game_dir == in.game_dir);
}

NXTEST(launcher_config_round_trip_comment_markers_in_path) {
    // a space before ';' or '#' would start a comment, so the writer quotes the value
    const std::string paths[] = {"C:\\My ;Games #2", "C:\\Games\\a;b#c", "C:\\Games\\#1"};
    for (const std::string& p : paths) {
        LauncherConfig in;
        in.game_dir = p;
        LauncherConfig out;
        CHECK(nxl::parse_launcher_config(nxl::format_launcher_config(in), &out));
        CHECK_MSG(out.game_dir == p, "comment marker path did not round-trip");
    }
}

NXTEST(launcher_config_bom_and_comments) {
    LauncherConfig out;
    const std::string bom_text = std::string("\xEF\xBB\xBF") + "; top comment\r\n# also a comment\r\n[launcher]\r\n" +
                                 "game_dir = D:\\MP ; my copy\r\n";
    CHECK(nxl::parse_launcher_config(bom_text, &out));
    CHECK(out.game_dir == "D:\\MP");

    // ';' or '#' not after a space is part of the path
    LauncherConfig hash;
    CHECK(nxl::parse_launcher_config("[launcher]\ngame_dir = D:\\Mp#1\n", &hash));
    CHECK(hash.game_dir == "D:\\Mp#1");
    LauncherConfig semi;
    CHECK(nxl::parse_launcher_config("[launcher]\ngame_dir=D:\\a;b\n", &semi));
    CHECK(semi.game_dir == "D:\\a;b");
}

NXTEST(launcher_config_unknown_keys_and_sections_ignored) {
    LauncherConfig out;
    const std::string text =
        "[launcher]\n"
        "language = en\n"
        "window_width = 1280\n"
        "game_dir = E:\\X\n"
        "[other]\n"
        "game_dir = G:\\Wrong\n";
    CHECK(nxl::parse_launcher_config(text, &out));
    CHECK(out.game_dir == "E:\\X");
}

NXTEST(launcher_config_missing_key_keeps_default) {
    LauncherConfig out;
    out.game_dir = "F:\\Default";
    CHECK(nxl::parse_launcher_config("[launcher]\nlanguage = en\n", &out));
    CHECK(out.game_dir == "F:\\Default");

    // the key under another section does not count
    CHECK(nxl::parse_launcher_config("[other]\ngame_dir = G:\\X\n", &out));
    CHECK(out.game_dir == "F:\\Default");

    // an empty value keeps the default too
    CHECK(nxl::parse_launcher_config("[launcher]\ngame_dir = ; nothing here\n", &out));
    CHECK(out.game_dir == "F:\\Default");
}

NXTEST(launcher_config_invalid_utf8_is_rejected) {
    LauncherConfig out;
    out.game_dir = "F:\\Default";
    CHECK(!nxl::parse_launcher_config("[launcher]\ngame_dir = \xC3\x28\n", &out));
    CHECK(out.game_dir == "F:\\Default");
}

// ---- game folder checks ----------------------------------------------------------------------------------------------

NXTEST(summary_ready_text) {
    GameStatus s;
    s.exefs = s.romfs = s.textures = s.shaders = s.setup_finished = true;
    CHECK(s.ready());
    CHECK(s.summary() == "Ready to play.");
}

NXTEST(game_dir_empty_folder_has_nothing) {
    TempDir t;
    const GameStatus s = nxl::check_game_dir(t.str());
    CHECK(!s.exefs);
    CHECK(!s.romfs);
    CHECK(!s.textures);
    CHECK(!s.shaders);
    CHECK(!s.setup_finished);
    CHECK(!s.ready());
    CHECK(s.summary() == "No game files in this folder yet. Run setup to create them from your own game dump.");
}

NXTEST(game_dir_empty_string_has_nothing) {
    const GameStatus s = nxl::check_game_dir("");
    CHECK(!s.ready());
    CHECK(s.summary() == "No game files in this folder yet. Run setup to create them from your own game dump.");
}

NXTEST(game_dir_empty_subfolders_do_not_count) {
    TempDir t;
    write_file(t.path / "exefs" / "main", "nso");
    fs::create_directories(t.path / "romfs");
    fs::create_directories(t.path / "bc7_cache");
    fs::create_directories(t.path / "shader_cache");
    const GameStatus s = nxl::check_game_dir(t.str());
    CHECK(s.exefs);
    CHECK(!s.romfs);
    CHECK(!s.textures);
    CHECK(!s.shaders);
    CHECK(!s.ready());
    CHECK(s.summary() == "Some game files are missing. Run setup again.");
}

NXTEST(game_dir_exefs_and_romfs_is_ready) {
    TempDir t;
    write_file(t.path / "exefs" / "main", "nso");
    write_file(t.path / "romfs" / "a.bin", "data");
    const GameStatus s = nxl::check_game_dir(t.str());
    CHECK(s.exefs);
    CHECK(s.romfs);
    CHECK(!s.textures);
    CHECK(!s.shaders);
    CHECK(!s.setup_finished);
    CHECK(s.ready());
    CHECK(s.summary() == "Game files found, but setup did not finish. Run setup again to finish it.");
}

NXTEST(game_dir_textures_and_shaders_without_state_file_did_not_finish) {
    TempDir t;
    write_file(t.path / "exefs" / "main", "nso");
    write_file(t.path / "romfs" / "a.bin", "data");
    write_file(t.path / "bc7_cache" / "tex_0001.bc7", "bc7");
    write_file(t.path / "shader_cache" / "spirv" / "s_0001.spv", "spv");
    const GameStatus s = nxl::check_game_dir(t.str());
    CHECK(s.ready());
    CHECK(s.textures);
    CHECK(s.shaders);
    CHECK(!s.setup_finished);
    CHECK(s.summary() == "Game files found, but setup did not finish. Run setup again to finish it.");
}

NXTEST(game_dir_finished_setup_is_ready_to_play) {
    TempDir t;
    write_file(t.path / "exefs" / "main", "nso");
    write_file(t.path / "romfs" / "a.bin", "data");
    write_file(t.path / "bc7_cache" / "tex_0001.bc7", "bc7");
    write_file(t.path / "shader_cache" / "spirv" / "s_0001.spv", "spv");
    write_file(t.path / "setup_state.tsv", kFinishedState);
    const GameStatus s = nxl::check_game_dir(t.str());
    CHECK(s.setup_finished);
    CHECK(s.ready());
    CHECK(s.summary() == "Ready to play.");
}

NXTEST(game_dir_state_file_must_have_the_setup_header_and_a_finished_line) {
    TempDir t;
    write_file(t.path / "exefs" / "main", "nso");
    write_file(t.path / "romfs" / "a.bin", "data");
    write_file(t.path / "bc7_cache" / "tex_0001.bc7", "bc7");
    write_file(t.path / "shader_cache" / "spirv" / "s_0001.spv", "spv");

    // the neutral state header counts as well as the first release's
    write_file(t.path / "setup_state.tsv", "nx_setup_state\t1\nmeta\tfinished\t1\n");
    CHECK(nxl::check_game_dir(t.str()).setup_finished);

    // other state file version: the setup tool starts fresh, so nothing counts as finished
    write_file(t.path / "setup_state.tsv", "mprs_setup_state\t2\nmeta\tfinished\t1\n");
    CHECK(!nxl::check_game_dir(t.str()).setup_finished);

    // the last finished line wins, as in the setup tool's loader
    write_file(t.path / "setup_state.tsv", "mprs_setup_state\t1\nmeta\tfinished\t1\nmeta\tfinished\t0\n");
    CHECK(!nxl::check_game_dir(t.str()).setup_finished);

    // a run that did not finish has no finished line
    write_file(t.path / "setup_state.tsv", "mprs_setup_state\t1\nmeta\tbc7_quality\t3\n");
    CHECK(!nxl::check_game_dir(t.str()).setup_finished);
}

NXTEST(game_dir_only_romfs_is_missing_files) {
    TempDir t;
    write_file(t.path / "romfs" / "a.bin", "data");
    const GameStatus s = nxl::check_game_dir(t.str());
    CHECK(!s.exefs);
    CHECK(!s.ready());
    CHECK(s.summary() == "Some game files are missing. Run setup again.");
}

// ---- paths and the settings file -------------------------------------------------------------------------------------

NXTEST(controls_path_is_inside_game_folder) {
    CHECK(nxl::controls_path("D:\\Game") == "D:\\Game\\controls.ini");
    CHECK(nxl::controls_path("D:\\Game\\") == "D:\\Game\\controls.ini");
}

NXTEST(launcher_config_save_then_load) {
    TempDir t;
    const std::string path = (t.path / "fixture_launcher.ini").string();

    LauncherConfig in;
    in.game_dir = "E:\\Games\\Some Game \u00e9";
    std::string err;
    CHECK(nxl::save_launcher_config(path, in, &err));
    CHECK(!fs::exists(path + ".tmp"));

    LauncherConfig out;
    CHECK(nxl::load_launcher_config(path, &out));
    CHECK(out.game_dir == in.game_dir);

    // a second save replaces the first
    in.game_dir = "F:\\Two";
    CHECK(nxl::save_launcher_config(path, in, &err));
    CHECK(nxl::load_launcher_config(path, &out));
    CHECK(out.game_dir == "F:\\Two");
}

NXTEST(launcher_config_missing_file_gives_default) {
    TempDir t;
    LauncherConfig out;
    out.game_dir = "stale";
    CHECK(!nxl::load_launcher_config((t.path / "does_not_exist.ini").string(), &out));
    CHECK(out.game_dir == nxl::default_game_dir());
}
