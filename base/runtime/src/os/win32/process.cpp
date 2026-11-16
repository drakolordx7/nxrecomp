// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::process on Windows (include/nxrt/os/process.h).
#include "nxrt/os/process.h"

#include <windows.h>
#include <bcrypt.h>
#include <stdlib.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

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

// ---- child processes ----------------------------------------------------------------------------------------------------------------

namespace {

// One job object for the process: every child that must not outlive it joins, and the system ends them when this process does.
HANDLE kill_job() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (!j) return static_cast<HANDLE>(nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(j, JobObjectExtendedLimitInformation, &lim, sizeof lim);
        return j;
    }();
    return job;
}

// One argument as CreateProcess's command line wants it (the rules of CommandLineToArgvW).
void append_arg(std::wstring& cl, const std::wstring& a) {
    if (!cl.empty()) cl += L' ';
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cl += a;
        return;
    }
    cl += L'"';
    for (size_t i = 0;; i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { i++; bs++; }
        if (i == a.size()) {
            cl.append(bs * 2, L'\\');
            break;
        }
        if (a[i] == L'"') cl.append(bs * 2 + 1, L'\\');
        else cl.append(bs, L'\\');
        cl += a[i];
    }
    cl += L'"';
}

}  // namespace

Child Child::spawn(const std::string& exe, const std::vector<std::string>& args, const SpawnOptions& opt) {
    Child c;
    std::wstring cl;
    append_arg(cl, str::widen(exe));
    for (const std::string& a : args) append_arg(cl, str::widen(a));
    STARTUPINFOW su{};
    su.cb = sizeof su;
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_SUSPENDED | NORMAL_PRIORITY_CLASS | (opt.hidden ? CREATE_NO_WINDOW : 0);
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &su, &pi)) return c;
    if (opt.kill_with_parent)
        if (HANDLE job = kill_job()) AssignProcessToJobObject(job, pi.hProcess);
    if (opt.cpu_mask) SetProcessAffinityMask(pi.hProcess, static_cast<DWORD_PTR>(opt.cpu_mask));
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    c.h_ = reinterpret_cast<intptr_t>(pi.hProcess);
    c.pid_ = pi.dwProcessId;
    return c;
}

bool Child::wait(uint32_t timeout_ms, int* exit_code) {
    if (!h_) return true;
    HANDLE p = reinterpret_cast<HANDLE>(h_);
    if (WaitForSingleObject(p, timeout_ms) != WAIT_OBJECT_0) return false;
    DWORD code = 0;
    GetExitCodeProcess(p, &code);
    if (exit_code) *exit_code = static_cast<int>(code);
    return true;
}

void Child::kill() {
    if (!h_) return;
    HANDLE p = reinterpret_cast<HANDLE>(h_);
    TerminateProcess(p, 1);
    WaitForSingleObject(p, 10000);
}

void Child::close() {
    if (!h_) return;
    kill();
    CloseHandle(reinterpret_cast<HANDLE>(h_));
    h_ = 0;
    pid_ = 0;
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
