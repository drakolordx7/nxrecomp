// SPDX-License-Identifier: GPL-3.0-or-later
#include "fileio.h"

#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mprs {
namespace {

bool is_sep(char c) { return c == '\\' || c == '/'; }

HANDLE as_handle(void* h) { return static_cast<HANDLE>(h); }

// True if the path names an existing folder.
bool is_dir(const std::string& utf8) {
    DWORD a = GetFileAttributesW(widen(utf8).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string parent_of(const std::string& p) {
    size_t sep = p.find_last_of("\\/");
    return sep == std::string::npos ? std::string() : p.substr(0, sep);
}

}  // namespace

// ---- AtomicFileWriter

AtomicFileWriter::AtomicFileWriter(const std::string& final_utf8_path)
    : final_path_(final_utf8_path), part_path_(final_utf8_path + ".part") {
    std::string dir = parent_of(final_utf8_path);
    if (!dir.empty() && !create_dirs(dir)) fail(Fail::Disk, "Cannot create the folder for \"%s\".", final_utf8_path.c_str());
    HANDLE h = CreateFileW(widen(part_path_).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) fail(Fail::Disk, "Cannot create \"%s\".", part_path_.c_str());
    handle_ = h;
}

AtomicFileWriter::~AtomicFileWriter() {
    if (handle_) CloseHandle(as_handle(handle_));
    if (!finished_) DeleteFileW(widen(part_path_).c_str());
}

void AtomicFileWriter::write(const void* data, size_t len) {
    if (!handle_) fail(Fail::Generic, "Internal: write after finish on \"%s\".", final_path_.c_str());
    const u8* p = static_cast<const u8*>(data);
    while (len) {
        DWORD chunk = len > 0x10000000u ? 0x10000000u : static_cast<DWORD>(len);
        DWORD written = 0;
        if (!WriteFile(as_handle(handle_), p, chunk, &written, nullptr) || written == 0) {
            DWORD err = GetLastError();
            if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) fail(Fail::Disk, "Not enough space on the drive.");
            fail(Fail::Disk, "Cannot write \"%s\".", part_path_.c_str());
        }
        sha_.update(p, written);
        p += written;
        len -= written;
        bytes_ += written;
    }
}

u64 AtomicFileWriter::bytes() const { return bytes_; }

std::array<u8, 32> AtomicFileWriter::finish() {
    if (finished_ || !handle_) fail(Fail::Generic, "Internal: finish called twice on \"%s\".", final_path_.c_str());
    if (!FlushFileBuffers(as_handle(handle_))) fail(Fail::Disk, "Cannot save \"%s\".", part_path_.c_str());
    CloseHandle(as_handle(handle_));
    handle_ = nullptr;
    if (!MoveFileExW(widen(part_path_).c_str(), widen(final_path_).c_str(), MOVEFILE_REPLACE_EXISTING))
        fail(Fail::Disk, "Cannot replace \"%s\".", final_path_.c_str());
    finished_ = true;
    return sha_.finish();
}

// ---- file checks and folders

bool file_size_is(const std::string& utf8, u64 size) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(widen(utf8).c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    u64 s = (static_cast<u64>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return s == size;
}

bool path_exists(const std::string& utf8) { return GetFileAttributesW(widen(utf8).c_str()) != INVALID_FILE_ATTRIBUTES; }

bool create_dirs(const std::string& dir_utf8) {
    std::string d = dir_utf8;
    // Trailing separators go, except on a drive root such as "D:\".
    while (d.size() > 1 && is_sep(d.back()) && !(d.size() == 3 && d[1] == ':')) d.pop_back();
    if (d.empty()) return false;
    if (is_dir(d)) return true;
    size_t sep = d.find_last_of("\\/");
    if (sep != std::string::npos && sep > 0 && !create_dirs(d.substr(0, sep))) return false;
    if (CreateDirectoryW(widen(d).c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS && is_dir(d);
}

u64 free_disk_bytes(const std::string& any_path_utf8) {
    std::string p = any_path_utf8;
    while (!is_dir(p)) {
        size_t sep = p.find_last_of("\\/");
        if (sep == std::string::npos) {
            p = ".";
            break;
        }
        p.erase(sep);
        if (p.empty()) p = "\\";
    }
    ULARGE_INTEGER avail{};
    if (!GetDiskFreeSpaceExW(widen(p).c_str(), &avail, nullptr, nullptr)) return 0;
    return static_cast<u64>(avail.QuadPart);
}

// ---- whole-file text helpers (bytes, not text: nothing is converted)

bool read_text_file(const std::string& utf8, std::string& out) {
    HANDLE h = CreateFileW(widen(utf8).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && sz.QuadPart < (1LL << 31);
    std::string buf;
    if (ok) {
        buf.assign(static_cast<size_t>(sz.QuadPart), '\0');
        size_t got = 0;
        while (got < buf.size()) {
            DWORD want = buf.size() - got > 0x10000000u ? 0x10000000u : static_cast<DWORD>(buf.size() - got);
            DWORD n = 0;
            if (!ReadFile(h, buf.data() + got, want, &n, nullptr) || n == 0) {
                ok = false;
                break;
            }
            got += n;
        }
    }
    CloseHandle(h);
    if (!ok) return false;
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
    return a.substr(0, ae) + "\\" + b.substr(bs);
}

std::string exe_dir() {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    std::wstring w(buf.data(), n);
    size_t sep = w.find_last_of(L"\\/");
    if (sep == std::wstring::npos) return {};
    return narrow(w.substr(0, sep));
}

int remove_part_files(const std::string& dir_utf8) {
    int removed = 0;
    std::wstring dir = widen(dir_utf8);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            removed += remove_part_files(narrow(full));
        } else if (name.size() > 5 && name.compare(name.size() - 5, 5, L".part") == 0) {
            if (DeleteFileW(full.c_str())) ++removed;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return removed;
}

}  // namespace mprs
