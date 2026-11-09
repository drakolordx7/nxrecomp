// SPDX-License-Identifier: GPL-3.0-or-later
// Configuration: an ini file plus environment-variable switches.
//   [section]
//   key = value        ; comment    # comment
// Lookup order for config_get_*("diag", "watchdog_seconds", def):
//   1. environment variable NXRT_DIAG_WATCHDOG_SECONDS   (NXRT_ + upper-case section + '_' + upper-case key)
//   2. the ini value (config_set values count as ini values)
//   3. the game's default (config_set_defaults: nxrt/game.h Info::config_defaults, the game exe's layout defaults)
//   4. the default
// Keys outside any section use the section name "" and the env name NXRT_<KEY>.
#pragma once
#include <cstdint>
#include <string>

namespace nxrt {

// Loads (replaces) the ini from a file. Returns false when the file cannot be read (the config stays empty).
bool config_load_file(const char* path);
// Loads from a string (tests). Replaces the current ini content.
void config_load_string(const char* text);
void config_clear();
// Programmatic override, as if it were in the ini (tests, command-line options). Env still wins over it.
void config_set(const char* section, const char* key, const char* value);
// The game's defaults: a layer below the ini (an ini value or the environment wins). It is kept when the ini is loaded, replaced or
// cleared, so it can be set before nxrt::init(). Values are merged into the layer; config_clear_defaults empties it.
void config_set_defaults(const char* ini_text);
void config_set_default(const char* section, const char* key, const char* value);
void config_clear_defaults();

std::string config_get_str(const char* section, const char* key, const char* def = "");
int64_t config_get_int(const char* section, const char* key, int64_t def = 0);      // decimal, 0x hex, optional k/m/g suffix not supported
double config_get_double(const char* section, const char* key, double def = 0.0);
bool config_get_bool(const char* section, const char* key, bool def = false);       // 1/0 true/false yes/no on/off, case-insensitive
bool config_has(const char* section, const char* key);                              // set by env, ini or the game defaults

// One value of ANOTHER ini file (the play-mode settings file, controls.ini), read with the same parser as the main ini: sections, quoting
// and comments behave identically. No environment lookup and no effect on the loaded ini. `def` when the file or the key is missing.
std::string config_file_get_str(const char* path, const char* section, const char* key, const char* def = "");

// Where a value came from: "env", "ini", "game" or "default". For the startup dump.
const char* config_source(const char* section, const char* key);
// Logs (Info, tag "config") every key read so far with its effective value and source.
void config_log_effective();
// The environment variable name used for a key, e.g. NXRT_DIAG_WATCHDOG_SECONDS.
std::string config_env_name(const char* section, const char* key);

}
