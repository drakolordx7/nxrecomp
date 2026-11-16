// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt::os::fs on Linux (include/nxrt/os/fs.h). Paths are UTF-8 already; the file system keeps them as bytes.
#define _FILE_OFFSET_BITS 64
#include "nxrt/os/fs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace nxrt::os::fs {
namespace {

std::string z(std::string_view s) { return std::string(s); }

// Whether `s` is well-formed UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
bool valid_utf8(const std::string& s) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        size_t len;
        uint32_t cp;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

int64_t seconds_of(const struct timespec& t) { return static_cast<int64_t>(t.tv_sec); }

}  // namespace

// ---- File ---------------------------------------------------------------------------------------------------------------------------

File File::open(std::string_view path, uint32_t flags) {
    File f;
    const bool w = (flags & (kWrite | kCreate | kCreateNew | kTruncate)) != 0;
    int o = O_CLOEXEC | (w ? ((flags & kRead) ? O_RDWR : O_WRONLY) : O_RDONLY);
    if (flags & kCreateNew) o |= O_CREAT | O_EXCL;
    else if (flags & kCreate) o |= O_CREAT;
    if (flags & kTruncate) o |= O_TRUNC;
    int fd;
    do fd = ::open(z(path).c_str(), o, 0666);
    while (fd < 0 && errno == EINTR);
    f.h_ = fd;
    return f;
}

void File::close() {
    if (h_ != -1) ::close(static_cast<int>(h_));
    h_ = -1;
}

bool File::read_at(uint64_t offset, void* dst, uint64_t n, uint64_t* got) const {
    if (got) *got = 0;
    uint8_t* d = static_cast<uint8_t*>(dst);
    uint64_t total = 0;
    while (n > 0) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(n, 0x40000000));
        const ssize_t r = ::pread(static_cast<int>(h_), d, chunk, static_cast<off_t>(offset));
        if (r < 0) {
            if (errno == EINTR) continue;
            if (got) *got = total;
            return false;
        }
        if (r == 0) break;
        d += r;
        offset += static_cast<uint64_t>(r);
        n -= static_cast<uint64_t>(r);
        total += static_cast<uint64_t>(r);
    }
    if (got) *got = total;
    return true;
}

bool File::write_at(uint64_t offset, const void* src, uint64_t n) const {
    const uint8_t* s = static_cast<const uint8_t*>(src);
    while (n > 0) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(n, 0x40000000));
        const ssize_t w = ::pwrite(static_cast<int>(h_), s, chunk, static_cast<off_t>(offset));
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) return false;
        s += w;
        offset += static_cast<uint64_t>(w);
        n -= static_cast<uint64_t>(w);
    }
    return true;
}

bool File::size(uint64_t* out) const {
    struct ::stat st;
    if (::fstat(static_cast<int>(h_), &st) != 0) return false;
    *out = static_cast<uint64_t>(st.st_size);
    return true;
}

bool File::set_size(uint64_t n, bool) const { return ::ftruncate(static_cast<int>(h_), static_cast<off_t>(n)) == 0; }

bool File::sync() const { return ::fsync(static_cast<int>(h_)) == 0; }

std::filesystem::path native_path(std::string_view path) { return std::filesystem::path(z(path)); }

std::FILE* fopen(std::string_view path, const char* mode) { return ::fopen(z(path).c_str(), mode); }

// ---- paths and folders --------------------------------------------------------------------------------------------------------------

bool stat(std::string_view path, Stat* out) {
    const std::string p = z(path);
    struct ::stat st;
    if (::stat(p.c_str(), &st) != 0) return false;
    if (out) {
        out->is_dir = S_ISDIR(st.st_mode);
        out->is_file = S_ISREG(st.st_mode);
        out->size = out->is_file ? static_cast<uint64_t>(st.st_size) : 0;
        out->modified = seconds_of(st.st_mtim);
        out->accessed = seconds_of(st.st_atim);
        out->created = out->modified;
#ifdef STATX_BTIME
        struct statx sx;
        if (::statx(AT_FDCWD, p.c_str(), 0, STATX_BTIME, &sx) == 0 && (sx.stx_mask & STATX_BTIME)) out->created = static_cast<int64_t>(sx.stx_btime.tv_sec);
#endif
    }
    return true;
}

bool list_dir(std::string_view path, std::vector<DirEntry>* out, size_t* bad) {
    namespace sf = std::filesystem;
    out->clear();
    std::error_code ec;
    sf::directory_iterator it(sf::path(z(path)), sf::directory_options::skip_permission_denied, ec);
    const sf::directory_iterator end;
    if (ec) return false;
    for (; !ec && it != end; it.increment(ec)) {
        const sf::directory_entry& e = *it;
        DirEntry x;
        x.name = e.path().filename().native();
        if (x.name.empty() || !valid_utf8(x.name)) {
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
    const std::string a = z(from), b = z(to);
    if (::rename(a.c_str(), b.c_str()) != 0) return false;
    if (durable) {   // the new name is in the folder's data: sync the folder
        const size_t slash = b.find_last_of('/');
        const std::string dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : b.substr(0, slash));
        const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }
    return true;
}

bool sync_path(std::string_view path) {
    File f = File::open(path, kRead);
    return f.ok() && f.sync();
}

std::string absolute(std::string_view path) {
    namespace sf = std::filesystem;
    std::error_code ec;
    sf::path a = sf::absolute(sf::path(z(path)), ec);
    if (ec) return z(path);
    return a.lexically_normal().native();
}

std::string current_dir() {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::current_path(ec);
    return ec ? std::string(".") : p.native();
}

bool set_current_dir(std::string_view path) { return ::chdir(z(path).c_str()) == 0; }

bool dir_writable(std::string_view dir) {
    std::string p = z(dir);
    if (p.empty()) p = ".";
    p += "/.nxrt_probe_" + std::to_string(static_cast<long>(::getpid()));
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    ::close(fd);
    ::unlink(p.c_str());
    return true;
}

// ---- case-insensitive lookups -------------------------------------------------------------------------------------------------------

namespace {

std::string fold(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return r;
}

struct Listing {
    int64_t mtime_ns = -1;
    std::unordered_map<std::string, std::string> by_fold;   // folded name -> the first spelling in sorted order
};

std::mutex g_case_mu;
std::unordered_map<std::string, Listing>& case_cache() {
    static std::unordered_map<std::string, Listing> c;
    return c;
}

// The entry of `dir` that matches `name` ignoring case; empty when there is none. Caller holds g_case_mu.
std::string match_in(const std::string& dir, const std::string& name) {
    const std::string d = dir.empty() ? "." : dir;
    struct ::stat st;
    if (::stat(d.c_str(), &st) != 0) return std::string();
    const int64_t mt = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000ll + st.st_mtim.tv_nsec;
    Listing& l = case_cache()[d];
    if (l.mtime_ns != mt) {
        std::vector<DirEntry> ents;
        l.by_fold.clear();
        l.mtime_ns = mt;
        if (list_dir(d, &ents)) {
            for (const DirEntry& e : ents) {
                auto ins = l.by_fold.emplace(fold(e.name), e.name);
                if (!ins.second && e.name < ins.first->second) ins.first->second = e.name;
            }
        }
    }
    auto it = l.by_fold.find(fold(name));
    return it == l.by_fold.end() ? std::string() : it->second;
}

}  // namespace

std::string resolve_case(std::string_view path) {
    const std::string orig = z(path);
    if (orig.empty() || ::access(orig.c_str(), F_OK) == 0) return orig;
    std::vector<std::string> parts;
    for (size_t i = 0; i < orig.size();) {
        size_t j = orig.find('/', i);
        if (j == std::string::npos) j = orig.size();
        if (j > i) parts.push_back(orig.substr(i, j - i));
        i = j + 1;
    }
    std::string cur = orig[0] == '/' ? "/" : "";
    std::lock_guard<std::mutex> lk(g_case_mu);
    for (size_t k = 0; k < parts.size(); ++k) {
        const std::string& comp = parts[k];
        const std::string cand = cur.empty() ? comp : (cur.back() == '/' ? cur + comp : cur + "/" + comp);
        if (comp == "." || comp == ".." || ::access(cand.c_str(), F_OK) == 0) {
            cur = cand;
            continue;
        }
        const std::string m = match_in(cur, comp);
        if (m.empty()) return orig;   // not found even ignoring case
        cur = cur.empty() ? m : (cur.back() == '/' ? cur + m : cur + "/" + m);
    }
    return cur.empty() ? orig : cur;
}

void clear_case_cache() {
    std::lock_guard<std::mutex> lk(g_case_mu);
    case_cache().clear();
}

}  // namespace nxrt::os::fs
