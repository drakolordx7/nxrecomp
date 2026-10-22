// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage.h"

#include <algorithm>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mprs {
namespace {

class FileStorage final : public IStorage {
public:
    FileStorage(HANDLE h, u64 size) : h_(h), size_(size) {}
    ~FileStorage() override { CloseHandle(h_); }
    u64 size() const override { return size_; }
    void read(u64 off, void* buf, size_t len) const override {
        if (off > size_ || len > size_ - off)
            fail(Fail::Input, "The file is shorter than it should be (read of %llu bytes at offset %llu past the end at %llu). "
                              "The dump is probably truncated or incomplete.",
                 (unsigned long long)len, (unsigned long long)off, (unsigned long long)size_);
        u8* p = static_cast<u8*>(buf);
        while (len > 0) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(len, 1u << 30));
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(off);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            DWORD got = 0;
            if (!ReadFile(h_, p, chunk, &got, &ov) || got == 0)
                fail(Fail::Generic, "Reading the file failed (Windows error %lu at offset %llu).", GetLastError(),
                     (unsigned long long)off);
            p += got;
            off += got;
            len -= got;
        }
    }
private:
    HANDLE h_;
    u64 size_;
};

class SubStorage final : public IStorage {
public:
    SubStorage(StoragePtr b, u64 off, u64 len) : base_(std::move(b)), off_(off), len_(len) {}
    u64 size() const override { return len_; }
    void read(u64 off, void* buf, size_t len) const override {
        if (off > len_ || len > len_ - off)
            fail(Fail::Input, "A part of the dump points past its own end (offset %llu, %llu bytes, size %llu).",
                 (unsigned long long)off, (unsigned long long)len, (unsigned long long)len_);
        base_->read(off_ + off, buf, len);
    }
private:
    StoragePtr base_;
    u64 off_, len_;
};

class MemoryStorage final : public IStorage {
public:
    explicit MemoryStorage(std::vector<u8> b) : b_(std::move(b)) {}
    u64 size() const override { return b_.size(); }
    void read(u64 off, void* buf, size_t len) const override {
        if (off > b_.size() || len > b_.size() - off) fail(Fail::Input, "Read past the end of an in-memory block.");
        if (len) std::memcpy(buf, b_.data() + off, len);
    }
private:
    std::vector<u8> b_;
};

}  // namespace

StoragePtr open_file_storage(const std::string& utf8_path) {
    HANDLE h = CreateFileW(widen(utf8_path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) fail(Fail::Input, "Cannot open \"%s\" (Windows error %lu).", utf8_path.c_str(), GetLastError());
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        fail(Fail::Generic, "Cannot get the size of \"%s\".", utf8_path.c_str());
    }
    return std::make_shared<FileStorage>(h, static_cast<u64>(sz.QuadPart));
}

StoragePtr make_sub_storage(StoragePtr base, u64 off, u64 len) {
    if (off > base->size() || len > base->size() - off)
        fail(Fail::Input, "A part of the dump points past the end of its container (offset %llu, %llu bytes, container %llu).",
             (unsigned long long)off, (unsigned long long)len, (unsigned long long)base->size());
    return std::make_shared<SubStorage>(std::move(base), off, len);
}

StoragePtr make_memory_storage(std::vector<u8> bytes) { return std::make_shared<MemoryStorage>(std::move(bytes)); }

std::vector<u8> read_all(const IStorage& s, u64 off, size_t len) {
    std::vector<u8> v(len);
    if (len) s.read(off, v.data(), len);
    return v;
}

}  // namespace mprs
