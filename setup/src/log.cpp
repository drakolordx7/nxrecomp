// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "common.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mprs {
namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
std::function<void(LogLevel, const std::string&)> g_sink;
LogLevel g_sink_min = LogLevel::Info;
}  // namespace

bool log_open(const std::string& utf8_path) {
    std::lock_guard lk(g_mu);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
    g_file = _wfopen(widen(utf8_path).c_str(), L"ab");
    return g_file != nullptr;
}

void log_close() {
    std::lock_guard lk(g_mu);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
}

void log_set_sink(std::function<void(LogLevel, const std::string&)> sink, LogLevel min_level) {
    std::lock_guard lk(g_mu);
    g_sink = std::move(sink);
    g_sink_min = min_level;
}

void logf(LogLevel level, const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    static const char* names[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
    char line[4300];
    std::snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond, t.wMilliseconds, names[static_cast<int>(level)], buf);
    std::function<void(LogLevel, const std::string&)> sink;
    {
        std::lock_guard lk(g_mu);
        if (g_file) { std::fputs(line, g_file); std::fflush(g_file); }
        if (g_sink && level >= g_sink_min) sink = g_sink;
    }
    if (sink) sink(level, buf);
}

}  // namespace mprs
