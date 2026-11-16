// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::fs on Windows (include/nxrt/os/fs.h). UTF-8 paths are converted to UTF-16 here and nowhere else.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include "nxrt/os/fs.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <filesystem>

#include "nxrt/strutil.h"

namespace nxrt::os::fs {
namespace {

constexpr size_t kLongPath = 240;   // host paths this long or longer get the \\?\ prefix

std::wstring absolute_w(const std::wstring& w) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetFullPathNameW(w.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
        if (n == 0) return w;
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(n + 1);
    }
}

// The path the Win32 calls take: backslashes, and absolute with the extended-length prefix when it is long.
std::wstring native(std::string_view p) {
    std::wstring w = str::widen(p);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    if (w.size() < kLongPath || w.compare(0, 4, L"\\\\?\\") == 0) return w;
    w = absolute_w(w);
    if (w.compare(0, 4, L"\\\\?\\") == 0) return w;
    if (w.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    return L"\\\\?\\" + w;
}

int64_t unix_seconds(const FILETIME& ft) {
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>(t / 10000000ull) - 11644473600ll;
}

HANDLE open_handle(const std::wstring& w, DWORD access, DWORD disposition) {
    return CreateFileW(w.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
}

}  // namespace

// ---- File ---------------------------------------------------------------------------------------------------------------------------

File File::open(std::string_view path, uint32_t flags) {
    File f;
    DWORD access = 0;
    if (flags & kRead) access |= GENERIC_READ;
    if (flags & (kWrite | kCreate | kCreateNew | kTruncate)) access |= GENERIC_WRITE;
    DWORD disp = OPEN_EXISTING;
    if (flags & kCreateNew) disp = CREATE_NEW;
    else if ((flags & kCreate) && (flags & kTruncate)) disp = CREATE_ALWAYS;
    else if (flags & kCreate) disp = OPEN_ALWAYS;
    else if (flags & kTruncate) disp = TRUNCATE_EXISTING;
    HANDLE h = open_handle(native(path), access, disp);
    f.h_ = reinterpret_cast<intptr_t>(h);
    return f;
}

void File::close() {
    if (h_ != -1) CloseHandle(reinterpret_cast<HANDLE>(h_));
    h_ = -1;
}

bool File::read_at(uint64_t offset, void* dst, uint64_t n, uint64_t* got) const {
    if (got) *got = 0;
    uint8_t* d = static_cast<uint8_t*>(dst);
    uint64_t total = 0;
    while (n > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(n, 0x40000000));
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD r = 0;
        if (!ReadFile(reinterpret_cast<HANDLE>(h_), d, chunk, &r, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            if (got) *got = total;
            return false;
        }
        if (r == 0) break;
        d += r;
        offset += r;
        n -= r;
        total += r;
        if (r < chunk) break;
    }
    if (got) *got = total;
    return true;
}

bool File::write_at(uint64_t offset, const void* src, uint64_t n) const {
    const uint8_t* s = static_cast<const uint8_t*>(src);
    while (n > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(n, 0x40000000));
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD w = 0;
        if (!WriteFile(reinterpret_cast<HANDLE>(h_), s, chunk, &w, &ov) || w == 0) return false;
        s += w;
        offset += w;
        n -= w;
    }
    return true;
}

bool File::size(uint64_t* out) const {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(reinterpret_cast<HANDLE>(h_), &li)) return false;
    *out = static_cast<uint64_t>(li.QuadPart);
    return true;
}

bool File::set_size(uint64_t n, bool sparse) const {
    HANDLE h = reinterpret_cast<HANDLE>(h_);
    if (sparse) {
        DWORD ret = 0;
        DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
    }
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(n);
    return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && SetEndOfFile(h);
}

bool File::sync() const { return FlushFileBuffers(reinterpret_cast<HANDLE>(h_)) != 0; }

std::filesystem::path native_path(std::string_view path) { return std::filesystem::path(native(path)); }

std::FILE* fopen(std::string_view path, const char* mode) { return _wfopen(native(path).c_str(), str::widen(mode).c_str()); }

// ---- paths and folders --------------------------------------------------------------------------------------------------------------

bool stat(std::string_view path, Stat* out) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(native(path).c_str(), GetFileExInfoStandard, &d)) return false;
    if (out) {
        out->is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out->is_file = !out->is_dir;
        out->size = out->is_dir ? 0 : ((static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow);
        out->created = unix_seconds(d.ftCreationTime);
        out->modified = unix_seconds(d.ftLastWriteTime);
        out->accessed = unix_seconds(d.ftLastAccessTime);
    }
    return true;
}

bool list_dir(std::string_view path, std::vector<DirEntry>* out, size_t* bad) {
    namespace sf = std::filesystem;
    out->clear();
    std::error_code ec;
    sf::directory_iterator it(sf::path(native(path)), sf::directory_options::skip_permission_denied, ec);
    const sf::directory_iterator end;
    if (ec) return false;
    for (; !ec && it != end; it.increment(ec)) {
        const sf::directory_entry& e = *it;
        DirEntry x;
        if (!str::narrow_strict(e.path().filename().wstring(), x.name) || x.name.empty()) {
            if (bad) ++*bad;
            continue;
        }
        std::error_code se;
        const sf::file_status ls = e.symlink_status(se);
        if (se) continue;
        x.is_symlink = sf::is_symlink(ls);
        const sf::file_status st = e.status(se);
        if (se) continue;
        x.is_dir = sf::is_directory(st);
        x.is_file = sf::is_regular_file(st);
        if (x.is_file) {
            std::error_code fe;
            x.size = e.file_size(fe);
            if (fe) continue;
        }
        out->push_back(std::move(x));
    }
    return true;
}

bool rename_replace(std::string_view from, std::string_view to, bool durable) {
    return MoveFileExW(native(from).c_str(), native(to).c_str(), MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0)) != 0;
}

bool sync_path(std::string_view path) {
    HANDLE h = open_handle(native(path), GENERIC_WRITE, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok;
}

std::string absolute(std::string_view path) {
    std::wstring w = str::widen(path);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return str::narrow(absolute_w(w));
}

std::string current_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(buf.size()), buf.data());
        if (n == 0) return ".";
        if (n < buf.size()) {
            buf.resize(n);
            return str::narrow(buf);
        }
        buf.resize(n + 1);
    }
}

bool set_current_dir(std::string_view path) { return SetCurrentDirectoryW(str::widen(path).c_str()) != 0; }

bool dir_writable(std::string_view dir) {
    std::wstring w = native(dir);
    if (!w.empty() && w.back() != L'\\') w += L'\\';
    w += L".nxrt_probe_" + std::to_wstring(GetCurrentProcessId());
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// Windows file systems ignore case themselves.
std::string resolve_case(std::string_view path) { return std::string(path); }
void clear_case_cache() {}

}  // namespace nxrt::os::fs
