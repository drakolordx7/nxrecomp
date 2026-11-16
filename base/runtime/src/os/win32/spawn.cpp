// SPDX-License-Identifier: GPL-3.0-or-later
// Windows implementation of nxrt/os/spawn.h.
#include "nxrt/os/spawn.h"

#include <windows.h>

#include <string>

namespace nxrt::os::process {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace

RunResult run_and_wait(const std::string& exe, const std::vector<std::string>& args, const std::vector<std::pair<std::string, std::string>>& env, uint32_t timeout_ms) {
    RunResult r;
    std::wstring cmd = L"\"" + widen(exe) + L"\"";
    for (const std::string& a : args) cmd += L" \"" + widen(a) + L"\"";
    // the environment block: ours without the names `env` sets, then `env`
    std::wstring block;
    if (LPWCH cur = GetEnvironmentStringsW()) {
        for (const wchar_t* p = cur; *p; p += wcslen(p) + 1) {
            bool replaced = false;
            for (const auto& kv : env) {
                const std::wstring k = widen(kv.first) + L"=";
                if (_wcsnicmp(p, k.c_str(), k.size()) == 0) replaced = true;
            }
            if (!replaced) block.append(p, wcslen(p) + 1);
        }
        FreeEnvironmentStringsW(cur);
    }
    for (const auto& kv : env) {
        block += widen(kv.first) + L"=" + widen(kv.second);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(widen(exe).c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, block.data(), nullptr, &si, &pi)) return r;
    r.started = true;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        r.exit_code = static_cast<int>(code);
    } else {
        r.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}

}  // namespace nxrt::os::process
