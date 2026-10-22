// SPDX-License-Identifier: GPL-3.0-or-later
// Small file helpers shared by the pipeline: atomic writes (write to "<name>.part", rename when complete), size and existence
// checks, folder creation, free disk space, UTF-8 path helpers. Plain Win32; paths are UTF-8 everywhere.
#pragma once
#include <array>
#include <string>

#include "common.h"
#include "crypto.h"

namespace mprs {

// Writes a file so that the final path only ever holds a complete file. Data goes to "<final>.part" and finish() renames it over
// the final path. If finish() was never reached (error, cancel, exception), the destructor closes and deletes the .part file and
// leaves any existing file at the final path untouched.
class AtomicFileWriter {
public:
    // Creates the parent folders and opens "<final>.part" for writing (truncating any old one). Throws Error(Disk).
    explicit AtomicFileWriter(const std::string& final_utf8_path);
    ~AtomicFileWriter();
    AtomicFileWriter(const AtomicFileWriter&) = delete;
    AtomicFileWriter& operator=(const AtomicFileWriter&) = delete;

    // Writes len bytes (direct WriteFile, no extra buffer). Updates the running SHA-256 and the byte count. Throws Error(Disk):
    // "Not enough space on the drive." when the disk is full, otherwise "Cannot write ...".
    void write(const void* data, size_t len);
    u64 bytes() const;
    // Flushes, closes and renames the .part file over the final path (replacing it). Returns the SHA-256 of everything written.
    // Call once; the writer is spent afterwards.
    std::array<u8, 32> finish();

private:
    std::string final_path_;
    std::string part_path_;
    void* handle_ = nullptr;   // a Win32 HANDLE, kept as void* so windows.h stays out of this header
    u64 bytes_ = 0;
    bool finished_ = false;
    Sha256 sha_;
};

// Exists and has exactly this size (a folder never matches).
bool file_size_is(const std::string& utf8, u64 size);
bool path_exists(const std::string& utf8);
// Creates the folder and any missing parents. True on success or if it already exists.
bool create_dirs(const std::string& dir_utf8);
// Free bytes available to the user on the drive of any_path (uses the nearest existing ancestor folder). 0 on failure.
u64 free_disk_bytes(const std::string& any_path_utf8);

// Reads a whole file as bytes into out. False if it cannot be opened or read.
bool read_text_file(const std::string& utf8, std::string& out);
// Writes content through AtomicFileWriter. False on any failure (the old file, if any, is kept).
bool write_text_file_atomic(const std::string& utf8, const std::string& content);

// Joins with one '\\' between the parts; extra separators at the join are removed. An empty part is skipped.
std::string path_join(const std::string& a, const std::string& b);
// Folder of the running executable, UTF-8, without a trailing separator.
std::string exe_dir();

// Deletes leftover "*.part" files (from a run that was killed) under dir, recursively. Returns how many were removed.
int remove_part_files(const std::string& dir_utf8);

}  // namespace mprs
