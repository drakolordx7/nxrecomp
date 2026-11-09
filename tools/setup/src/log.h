// SPDX-License-Identifier: GPL-3.0-or-later
// Process-wide log: a file (UTF-8, one line per message, local time) plus an optional sink callback (CLI console / GUI list).
// Thread-safe. Never log key bytes.
#pragma once
#include <functional>
#include <string>

namespace nxs {

enum class LogLevel { Debug, Info, Warn, Error };

// Opens (append) the log file. Returns false if it cannot be opened (logging then only goes to the sink).
bool log_open(const std::string& utf8_path);
void log_close();
// The sink gets every message at or above `min_level` (called from whichever thread logs; keep it quick).
void log_set_sink(std::function<void(LogLevel, const std::string&)> sink, LogLevel min_level = LogLevel::Info);

void logf(LogLevel level, const char* fmt, ...);
#define NXS_LOGI(...) ::nxs::logf(::nxs::LogLevel::Info, __VA_ARGS__)
#define NXS_LOGW(...) ::nxs::logf(::nxs::LogLevel::Warn, __VA_ARGS__)
#define NXS_LOGE(...) ::nxs::logf(::nxs::LogLevel::Error, __VA_ARGS__)
#define NXS_LOGD(...) ::nxs::logf(::nxs::LogLevel::Debug, __VA_ARGS__)

}  // namespace nxs
