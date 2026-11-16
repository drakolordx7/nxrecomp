// SPDX-License-Identifier: GPL-3.0-or-later
#include "fileio.h"

#include <cerrno>
#include <cstdint>
#include <fstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace nxs {
namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
bool is_sep(char c) { return c == '\\' || c == '/'; }
constexpr char kSep = '\\';
#else
bool is_sep(char c) { return c == '/'; }
constexpr char kSep = '/';
#endif

bool is_dir(const std::string& utf8) {
    std::error_code ec;
    return fs::is_directory(fs_path(utf8), ec);
}

std::string parent_of(const std::string& p) {
    size_t sep = p.find_last_of(kSep == '\\' ? "\\/" : "/");
    return sep == std::string::npos ? std::string() : p.substr(0, sep);
}

}  // namespace

std::filesystem::path fs_path(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

std::string path_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ---- AtomicFileWriter

namespace {
// The part file's handle lives in a void*: a HANDLE on Windows, the descriptor + 1 on POSIX (so that a valid handle is never null).
#ifdef _WIN32
HANDLE as_handle(void* h) { return static_cast<HANDLE>(h); }
#else
int as_fd(void* h) { return static_cast<int>(reinterpret_cast<intptr_t>(h)) - 1; }
#endif
}  // namespace

AtomicFileWriter::AtomicFileWriter(const std::string& final_utf8_path)
    : final_path_(final_utf8_path), part_path_(final_utf8_path + ".part") {
    std::string dir = parent_of(final_utf8_path);
    if (!dir.empty() && !create_dirs(dir)) fail(Fail::Disk, "Cannot create the folder for \"%s\".", final_utf8_path.c_str());
#ifdef _WIN32
    HANDLE h = CreateFileW(widen(part_path_).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) fail(Fail::Disk, "Cannot create \"%s\".", part_path_.c_str());
    handle_ = h;
#else
    int fd = ::open(part_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) fail(Fail::Disk, "Cannot create \"%s\".", part_path_.c_str());
    handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd) + 1);
#endif
}

AtomicFileWriter::~AtomicFileWriter() {
#ifdef _WIN32
    if (handle_) CloseHandle(as_handle(handle_));
    if (!finished_) DeleteFileW(widen(part_path_).c_str());
#else
    if (handle_) ::close(as_fd(handle_));
    if (!finished_) {
        std::error_code ec;
        fs::remove(fs_path(part_path_), ec);
    }
#endif
}

void AtomicFileWriter::write(const void* data, size_t len) {
    if (!handle_) fail(Fail::Generic, "Internal: write after finish on \"%s\".", final_path_.c_str());
    const u8* p = static_cast<const u8*>(data);
    while (len) {
#ifdef _WIN32
        DWORD chunk = len > 0x10000000u ? 0x10000000u : static_cast<DWORD>(len);
        DWORD written = 0;
        if (!WriteFile(as_handle(handle_), p, chunk, &written, nullptr) || written == 0) {
            DWORD err = GetLastError();
            if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) fail(Fail::Disk, "Not enough space on the drive.");
            fail(Fail::Disk, "Cannot write \"%s\".", part_path_.c_str());
        }
#else
        const size_t chunk = len > 0x10000000u ? 0x10000000u : len;
        const ssize_t w = ::write(as_fd(handle_), p, chunk);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            if (w < 0 && (errno == ENOSPC || errno == EDQUOT)) fail(Fail::Disk, "Not enough space on the drive.");
            fail(Fail::Disk, "Cannot write \"%s\".", part_path_.c_str());
        }
        const size_t written = static_cast<size_t>(w);
#endif
        sha_.update(p, written);
        p += written;
        len -= written;
        bytes_ += written;
    }
}

u64 AtomicFileWriter::bytes() const { return bytes_; }

std::array<u8, 32> AtomicFileWriter::finish() {
    if (finished_ || !handle_) fail(Fail::Generic, "Internal: finish called twice on \"%s\".", final_path_.c_str());
#ifdef _WIN32
    if (!FlushFileBuffers(as_handle(handle_))) fail(Fail::Disk, "Cannot save \"%s\".", part_path_.c_str());
    CloseHandle(as_handle(handle_));
    handle_ = nullptr;
    if (!MoveFileExW(widen(part_path_).c_str(), widen(final_path_).c_str(), MOVEFILE_REPLACE_EXISTING))
        fail(Fail::Disk, "Cannot replace \"%s\".", final_path_.c_str());
#else
    if (::fsync(as_fd(handle_)) != 0) fail(Fail::Disk, "Cannot save \"%s\".", part_path_.c_str());
    ::close(as_fd(handle_));
    handle_ = nullptr;
    if (std::rename(part_path_.c_str(), final_path_.c_str()) != 0) fail(Fail::Disk, "Cannot replace \"%s\".", final_path_.c_str());
#endif
    finished_ = true;
    return sha_.finish();
}

// ---- file checks and folders

bool file_size_is(const std::string& utf8, u64 size) {
    std::error_code ec;
    const fs::path p = fs_path(utf8);
    if (!fs::exists(p, ec) || fs::is_directory(p, ec)) return false;
    const auto s = fs::file_size(p, ec);
    return !ec && static_cast<u64>(s) == size;
}

bool path_exists(const std::string& utf8) {
    std::error_code ec;
    return fs::exists(fs_path(utf8), ec);
}

bool create_dirs(const std::string& dir_utf8) {
    std::string d = dir_utf8;
    // Trailing separators go, except on a root such as "D:\" or "/".
    while (d.size() > 1 && is_sep(d.back()) && !(d.size() == 3 && d[1] == ':')) d.pop_back();
    if (d.empty()) return false;
    if (is_dir(d)) return true;
    std::error_code ec;
    fs::create_directories(fs_path(d), ec);
    return !ec || is_dir(d);
}

u64 free_disk_bytes(const std::string& any_path_utf8) {
    std::error_code ec;
    fs::path p = fs_path(any_path_utf8);
    while (!p.empty() && !fs::is_directory(p, ec)) {
        fs::path up = p.parent_path();
        if (up == p) break;
        p = up;
    }
    if (p.empty()) p = ".";
    const fs::space_info si = fs::space(p, ec);
    return ec ? 0 : static_cast<u64>(si.available);
}

// ---- whole-file text helpers (bytes, not text: nothing is converted)

bool read_text_file(const std::string& utf8, std::string& out) {
    std::error_code ec;
    const fs::path p = fs_path(utf8);
    if (!fs::is_regular_file(p, ec)) return false;
    const auto size = fs::file_size(p, ec);
    if (ec || size >= (std::uintmax_t(1) << 31)) return false;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::string buf(static_cast<size_t>(size), '\0');
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (static_cast<size_t>(f.gcount()) != buf.size()) return false;
    out = std::move(buf);
    return true;
}

bool write_text_file_atomic(const std::string& utf8, const std::string& content) {
    try {
        AtomicFileWriter w(utf8);
        w.write(content.data(), content.size());
        w.finish();
        return true;
    } catch (const Error&) {
        return false;
    }
}

// ---- paths

std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    size_t ae = a.size();
    while (ae > 0 && is_sep(a[ae - 1])) --ae;
    size_t bs = 0;
    while (bs < b.size() && is_sep(b[bs])) ++bs;
    if (bs == b.size()) return a;
    return a.substr(0, ae) + kSep + b.substr(bs);
}

std::string native_path(std::string p) {
#ifdef _WIN32
    for (char& c : p) if (c == '/') c = '\\';
#endif
    return p;
}

std::string exe_dir() {
#ifdef _WIN32
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    std::wstring w(buf.data(), n);
    size_t sep = w.find_last_of(L"\\/");
    if (sep == std::wstring::npos) return {};
    return narrow(w.substr(0, sep));
#else
    std::vector<char> buf(4096);
    for (;;) {
        const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size());
        if (n < 0) return {};
        if (static_cast<size_t>(n) < buf.size()) {
            const std::string path(buf.data(), static_cast<size_t>(n));
            const size_t sep = path.find_last_of('/');
            return sep == std::string::npos ? std::string() : path.substr(0, sep);
        }
        buf.resize(buf.size() * 2);
    }
#endif
}

int remove_part_files(const std::string& dir_utf8) {
    std::vector<fs::path> parts;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(fs_path(dir_utf8), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string name = path_utf8(it->path().filename());
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0) parts.push_back(it->path());
    }
    int removed = 0;
    for (const fs::path& p : parts) {
        std::error_code e2;
        if (fs::remove(p, e2)) ++removed;
    }
    return removed;
}

}  // namespace nxs
