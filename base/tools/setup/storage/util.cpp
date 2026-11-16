// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdarg>
#include <cstdio>

#include "common.h"

namespace nxs {

void fail(Fail kind, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    throw Error(kind, buf);
}

std::string hex_encode(const void* data, size_t n, bool upper) {
    static const char* lo = "0123456789abcdef";
    static const char* up = "0123456789ABCDEF";
    const char* d = upper ? up : lo;
    const u8* p = static_cast<const u8*>(data);
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = d[p[i] >> 4];
        s[2 * i + 1] = d[p[i] & 15];
    }
    return s;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_decode(std::string_view s, u8* out, size_t n) {
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        int a = hexval(s[2 * i]), b = hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = static_cast<u8>(a << 4 | b);
    }
    return true;
}

// UTF-8 <-> wchar_t (UTF-16 on Windows, UTF-32 elsewhere). Invalid input gives U+FFFD, as the Windows converters do.
namespace {

void append_wide(std::wstring& w, uint32_t cp) {
    if constexpr (sizeof(wchar_t) == 2) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            w += static_cast<wchar_t>(0xD800 + (cp >> 10));
            w += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
            return;
        }
    }
    w += static_cast<wchar_t>(cp);
}

}  // namespace

std::wstring widen(std::string_view s) {
    std::wstring w;
    w.reserve(s.size());
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        uint32_t cp = c;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else if (c >= 0x80) { append_wide(w, 0xFFFD); ++i; continue; }
        bool ok = i + len <= n;
        for (size_t k = 1; ok && k < len; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (d & 0x3F);
        }
        if (ok && ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)))) ok = false;
        if (!ok) { append_wide(w, 0xFFFD); ++i; continue; }
        append_wide(w, cp);
        i += len;
    }
    return w;
}

std::string narrow(std::wstring_view w) {
    std::string s;
    s.reserve(w.size());
    const size_t n = w.size();
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = static_cast<uint32_t>(w[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && static_cast<uint32_t>(w[i + 1]) >= 0xDC00 && static_cast<uint32_t>(w[i + 1]) <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(w[i + 1]) - 0xDC00);
                ++i;
            }
        }
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
        if (cp < 0x80) s += static_cast<char>(cp);
        else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { s += static_cast<char>(0xF0 | (cp >> 18)); s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    return s;
}

}  // namespace nxs
