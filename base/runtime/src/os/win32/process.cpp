// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::process on Windows (include/nxrt/os/process.h).
#include "nxrt/os/process.h"

#include <windows.h>
#include <bcrypt.h>
#include <stdlib.h>

#include <algorithm>
#include <cstdio>

#include "nxrt/os/fs.h"
#include "nxrt/strutil.h"

#pragma comment(lib, "bcrypt")

namespace nxrt::os::process {

uint32_t pid() { return GetCurrentProcessId(); }

bool is_alive(uint32_t pid) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return alive;
}

[[noreturn]] void terminate(int code) {
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    for (;;) {}
}

std::string exe_path() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return std::string();
    buf.resize(n);
    return str::narrow(buf);
}

std::string exe_dir() {
    const std::string p = exe_path();
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

std::optional<std::string> getenv(const char* name) {
    const std::wstring w = str::widen(name);
    const DWORD n = GetEnvironmentVariableW(w.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring v(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(w.c_str(), v.data(), n);
    if (got == 0 || got >= n) return std::nullopt;
    v.resize(got);
    return str::narrow(v);
}

std::string home_dir() {
    const auto h = getenv("USERPROFILE");
    return h ? *h : std::string(".");
}

std::string user_data_dir(const char* app) {
    const auto a = getenv("APPDATA");
    return std::string(a && !a->empty() ? *a : std::string(".")) + "\\" + (app && *app ? app : "nxrt");
}

std::string user_config_dir(const char* app) { return user_data_dir(app); }

std::string portable_dir(const char* app) {
    static const std::string dir = exe_dir();
    static const bool writable = fs::dir_writable(dir);
    return writable ? dir : user_data_dir(app);
}

std::string default_save_dir(const char* app) { return user_data_dir(app) + "\\save"; }

std::string ui_language() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) <= 0) return std::string();
    return str::narrow(name);
}

bool random_bytes(void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        const ULONG c = static_cast<ULONG>(std::min<size_t>(n, 0x10000000));
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, p, c, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return false;
        p += c;
        n -= c;
    }
    return true;
}

NamedLock NamedLock::acquire(const std::string& name, bool wait) {
    std::wstring w = L"Local\\" + str::widen(name);
    std::replace(w.begin() + 6, w.end(), L'\\', L'_');
    NamedLock l;
    HANDLE m = CreateMutexW(nullptr, FALSE, w.c_str());
    if (!m) return l;
    const DWORD r = WaitForSingleObject(m, wait ? INFINITE : 0);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) l.h_ = reinterpret_cast<intptr_t>(m);
    else CloseHandle(m);
    return l;
}

void NamedLock::release() {
    if (!h_) return;
    HANDLE m = reinterpret_cast<HANDLE>(h_);
    ReleaseMutex(m);
    CloseHandle(m);
    h_ = 0;
}

void error_box(const char* title, const char* text) { MessageBoxA(nullptr, text, title, MB_OK | MB_ICONERROR); }

void attach_parent_console() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((!out || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
}

}  // namespace nxrt::os::process
