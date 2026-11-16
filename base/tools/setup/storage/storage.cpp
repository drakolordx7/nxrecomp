// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage.h"

#include <algorithm>
#include <cstring>

#include <filesystem>
#include <fstream>
#include <mutex>

namespace nxs {
namespace {

// A file read through the standard library: portable, nothing here touches the operating system directly. Reads at an offset take a lock (one
// stream, one position); the setup tool reads one file at a time in big blocks, where the lock costs nothing.
class FileStorage final : public IStorage {
public:
    FileStorage(std::ifstream f, u64 size) : f_(std::move(f)), size_(size) {}
    u64 size() const override { return size_; }
    void read(u64 off, void* buf, size_t len) const override {
        if (off > size_ || len > size_ - off)
            fail(Fail::Input, "The file is shorter than it should be (read of %llu bytes at offset %llu past the end at %llu). "
                              "The dump is probably truncated or incomplete.",
                 (unsigned long long)len, (unsigned long long)off, (unsigned long long)size_);
        if (len == 0) return;
        std::lock_guard<std::mutex> lk(mu_);
        f_.clear();
        f_.seekg(static_cast<std::streamoff>(off), std::ios::beg);
        f_.read(static_cast<char*>(buf), static_cast<std::streamsize>(len));
        if (static_cast<size_t>(f_.gcount()) != len)
            fail(Fail::Generic, "Reading the file failed (at offset %llu).", (unsigned long long)off);
    }
private:
    mutable std::ifstream f_;
    mutable std::mutex mu_;
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
    const std::filesystem::path p(std::u8string(utf8_path.begin(), utf8_path.end()));
    std::ifstream f(p, std::ios::binary);
    if (!f) fail(Fail::Input, "Cannot open \"%s\".", utf8_path.c_str());
    std::error_code ec;
    const u64 size = static_cast<u64>(std::filesystem::file_size(p, ec));
    if (ec) fail(Fail::Generic, "Cannot get the size of \"%s\".", utf8_path.c_str());
    return std::make_shared<FileStorage>(std::move(f), size);
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

}  // namespace nxs
