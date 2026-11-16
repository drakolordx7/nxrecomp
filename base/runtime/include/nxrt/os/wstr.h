// SPDX-License-Identifier: GPL-3.0-or-later
// UTF-8 <-> UTF-16 conversion for the Windows API (nxrt::str::widen / narrow, reached through nxrt/strutil.h). Header only; empty on other
// systems, where there is no wide-character API to call. This header is part of the os layer: it may include <windows.h>.
#pragma once
#ifdef _WIN32
#include <windows.h>

#include <string>
#include <string_view>

namespace nxrt::str {

// Invalid input gives replacement characters (narrow) or a partial result (widen); empty input gives an empty string.
inline std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::string narrow(const wchar_t* w) { return w ? narrow(std::wstring_view(w)) : std::string(); }

inline std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// False (and `out` empty) when the input is not valid; empty input gives true and an empty result.
inline bool narrow_strict(std::wstring_view w, std::string& out) {
    out.clear();
    if (w.empty()) return true;
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr) == n) return true;
    out.clear();
    return false;
}
inline bool widen_strict(std::string_view s, std::wstring& out) {
    out.clear();
    if (s.empty()) return true;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n) == n) return true;
    out.clear();
    return false;
}

}  // namespace nxrt::str
#endif
