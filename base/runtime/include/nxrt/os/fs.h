// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::fs: files, folders and paths of the host. Every path is UTF-8 (on Windows it is converted to UTF-16 inside src/os/win32/fs.cpp, and
// paths of 240 characters or more get the \\?\ prefix there); '/' is accepted as a separator on both systems. Nothing here throws.
// Everything above this header is platform neutral: see README.md in this folder.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nxrt::os::fs {

// ---- open files -------------------------------------------------------------------------------------------------------------------

enum OpenFlags : uint32_t {
    kRead = 1,
    kWrite = 2,
    kCreate = 4,      // with kWrite: create the file when it is missing
    kCreateNew = 8,   // with kWrite: create the file, fail when it exists
    kTruncate = 16,   // with kWrite: drop the old content
};

// One open file with positioned I/O (no file pointer: reads and writes at different offsets may run on different threads at once). Move-only;
// closes in the destructor. Other processes can read, write and delete the file while it is open (Windows: all three share modes are set).
class File {
public:
    File() = default;
    File(File&& o) noexcept : h_(o.h_) { o.h_ = -1; }
    File& operator=(File&& o) noexcept {
        if (this != &o) {
            close();
            h_ = o.h_;
            o.h_ = -1;
        }
        return *this;
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() { close(); }

    // An open file, or one for which ok() is false.
    static File open(std::string_view path, uint32_t flags);

    bool ok() const { return h_ != -1; }
    void close();
    // Reads up to `n` bytes at `offset` (files of any size). True when no error occurred, *got is the number of bytes read: less than `n` only
    // at the end of the file. `got` may be null.
    bool read_at(uint64_t offset, void* dst, uint64_t n, uint64_t* got = nullptr) const;
    // Reads exactly `n` bytes; false on an error or when the file is shorter.
    bool read_exact(uint64_t offset, void* dst, uint64_t n) const {
        uint64_t got = 0;
        return read_at(offset, dst, n, &got) && got == n;
    }
    bool write_at(uint64_t offset, const void* src, uint64_t n) const;
    bool size(uint64_t* out) const;
    // Sets the length (extends with zeros). `sparse`: mark the file sparse first, so a huge extension takes no disk space (NTFS needs the flag;
    // Linux file systems do it by themselves).
    bool set_size(uint64_t n, bool sparse = false) const;
    // Everything written so far reaches the disk.
    bool sync() const;

private:
    intptr_t h_ = -1;   // HANDLE or file descriptor
};

// A std::filesystem::path for a UTF-8 path that the host accepts at any length (Windows: converted, and with the extended-length prefix when the
// path is long). For code that uses std::filesystem on files whose names may be very long (tests); everything else passes UTF-8 to this API.
std::filesystem::path native_path(std::string_view path);

// A C stream on a UTF-8 path (fopen, with the same modes: "rb", "wb+", ...).
std::FILE* fopen(std::string_view path, const char* mode);

// ---- paths and folders --------------------------------------------------------------------------------------------------------------

struct Stat {
    bool is_dir = false;
    bool is_file = false;
    uint64_t size = 0;
    int64_t created = 0;    // Unix seconds; where the system keeps no creation time (older Linux file systems) the modification time
    int64_t modified = 0;
    int64_t accessed = 0;
};
// False when there is nothing at `path`. Links are followed.
bool stat(std::string_view path, Stat* out = nullptr);
inline bool exists(std::string_view path) { return stat(path); }
inline bool is_file(std::string_view path) {
    Stat s;
    return stat(path, &s) && s.is_file;
}
inline bool is_dir(std::string_view path) {
    Stat s;
    return stat(path, &s) && s.is_dir;
}

struct DirEntry {
    std::string name;
    bool is_dir = false;       // what the entry is after following a link
    bool is_file = false;
    bool is_symlink = false;
    uint64_t size = 0;         // of a file
};
// The entries of a folder, in no particular order ("." and ".." are not listed). Entries that cannot be read are left out, names that are not
// valid UTF-8 are counted in *bad and left out. False when the folder cannot be read.
bool list_dir(std::string_view path, std::vector<DirEntry>* out, size_t* bad = nullptr);

// Moves `from` to `to`, replacing an existing file at `to`, in one step on the same volume. `durable`: the move is on the disk when it returns
// (Windows MOVEFILE_WRITE_THROUGH; the folder is synced on Linux).
bool rename_replace(std::string_view from, std::string_view to, bool durable = false);
// Flushes a file by path (open, sync, close). False when it cannot be opened or synced.
bool sync_path(std::string_view path);

// The absolute form of `path` (relative to the current folder), normalised: no "." or ".." parts; Windows paths come back with backslashes.
// A path that cannot be resolved comes back unchanged.
std::string absolute(std::string_view path);
std::string current_dir();
bool set_current_dir(std::string_view path);
// Whether files can be created in the folder `dir` (a probe file is made and removed).
bool dir_writable(std::string_view dir);

// ---- case-insensitive lookups -------------------------------------------------------------------------------------------------------
// The guest sees a case-sensitive file system (Horizon); a game, a mod or an ini may spell a path with other capitalisation than the file on
// disk. Windows file systems ignore case already (the path comes back unchanged); on Linux this resolves it: when the exact spelling is not
// there, each part is replaced by the entry of its folder that matches ignoring ASCII case (the first in sorted order when several do). A
// path that exists exactly, or has no match, comes back unchanged. Folder listings are cached and read again when the folder's modification
// time changes, so a miss costs one listing per folder. Thread safe.
std::string resolve_case(std::string_view path);
void clear_case_cache();

}  // namespace nxrt::os::fs
