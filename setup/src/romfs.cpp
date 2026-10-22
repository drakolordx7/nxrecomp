// SPDX-License-Identifier: GPL-3.0-or-later
#include "romfs.h"

#include <utility>

namespace mprs {
namespace {

constexpr u64 kHeaderSize = 0x50;
constexpr u32 kNone = 0xFFFFFFFF;
constexpr unsigned kMaxDepth = 256;
constexpr u64 kDirFixed = 0x18;    // parent, next sibling, first child dir, first child file, next hash, name length
constexpr u64 kFileFixed = 0x20;   // parent, next sibling, data offset, data size, next hash, name length

u32 rd32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u64 rd64(const u8* p) { return u64(rd32(p)) | u64(rd32(p + 4)) << 32; }

// off + len lies inside total. Written so no sum can overflow.
bool fits(u64 off, u64 len, u64 total) { return off <= total && len <= total - off; }

// Name rules for folders and files below the root. The root itself has an empty name and is not checked.
void check_name(const std::string& n, const char* what) {
    if (n.empty()) fail(Fail::Input, "The RomFS is damaged: a %s has no name.", what);
    if (n == "." || n == "..")
        fail(Fail::Input, "The RomFS is damaged: a %s is named \"%s\", which is not allowed.", what, n.c_str());
    for (char c : n) {
        if (c == '/' || c == '\\' || c == ':' || c == '\0')
            fail(Fail::Input, "The RomFS is damaged: a %s has a name with a character that is not allowed (\"%s\").", what, n.c_str());
    }
}

std::string join(const std::string& dir, const std::string& name) { return dir.empty() ? name : dir + "/" + name; }

struct DirRec {
    u32 next_sibling;
    u32 first_child_dir;
    u32 first_child_file;
    std::string name;
};

struct FileRec {
    u32 next_sibling;
    u64 data_offset;
    u64 data_size;
    std::string name;
};

// Walks the tree from the root using only the child and sibling links. Every entry may be visited once, which rules out loops.
class Walker {
public:
    Walker(StoragePtr image, u64 dir_off, u64 dir_size, u64 file_off, u64 file_size, u64 data_off)
        : image_(std::move(image)), dir_off_(dir_off), dir_size_(dir_size), file_off_(file_off), file_size_(file_size),
          data_off_(data_off), img_size_(image_->size()), dir_seen_(size_t(dir_size)), file_seen_(size_t(file_size)) {}

    void run() {
        const DirRec root = read_dir(0, true);
        mark_dir(0);
        walk(root, "", 0);
    }

    std::vector<RomfsFile> files;
    std::vector<std::string> dirs;
    u64 total = 0;

private:
    std::string read_name(u64 abs, u64 len) {
        std::string s(size_t(len), '\0');
        if (len) image_->read(abs, s.data(), size_t(len));
        return s;
    }

    DirRec read_dir(u32 o, bool is_root) {
        if (!fits(o, kDirFixed, dir_size_))
            fail(Fail::Input, "The RomFS is damaged: a folder entry points outside the folder table.");
        u8 h[kDirFixed];
        image_->read(dir_off_ + o, h, sizeof h);
        DirRec r{rd32(h + 4), rd32(h + 8), rd32(h + 0xC), {}};
        const u32 name_len = rd32(h + 0x14);
        if (!fits(o + kDirFixed, name_len, dir_size_))
            fail(Fail::Input, "The RomFS is damaged: a folder name runs past the end of the folder table.");
        r.name = read_name(dir_off_ + o + kDirFixed, name_len);
        if (!is_root) check_name(r.name, "folder");
        return r;
    }

    FileRec read_file(u32 o) {
        if (!fits(o, kFileFixed, file_size_))
            fail(Fail::Input, "The RomFS is damaged: a file entry points outside the file table.");
        u8 h[kFileFixed];
        image_->read(file_off_ + o, h, sizeof h);
        FileRec r{rd32(h + 4), rd64(h + 8), rd64(h + 0x10), {}};
        const u32 name_len = rd32(h + 0x1C);
        if (!fits(o + kFileFixed, name_len, file_size_))
            fail(Fail::Input, "The RomFS is damaged: a file name runs past the end of the file table.");
        r.name = read_name(file_off_ + o + kFileFixed, name_len);
        check_name(r.name, "file");
        return r;
    }

    void mark_dir(u32 o) {
        if (dir_seen_[o])
            fail(Fail::Input, "The RomFS is damaged: the folder tree loops back on itself (a folder is reached twice).");
        dir_seen_[o] = true;
    }

    void mark_file(u32 o) {
        if (file_seen_[o])
            fail(Fail::Input, "The RomFS is damaged: the file list loops back on itself (a file is reached twice).");
        file_seen_[o] = true;
    }

    // Files of this folder first (sibling order), then each subfolder depth first.
    void walk(const DirRec& d, const std::string& path, unsigned depth) {
        for (u32 f = d.first_child_file; f != kNone;) {
            const FileRec fr = read_file(f);
            mark_file(f);
            if (!fits(data_off_, fr.data_offset, img_size_) || !fits(data_off_ + fr.data_offset, fr.data_size, img_size_))
                fail(Fail::Input, "The RomFS is damaged: file \"%s\" has data that runs past the end of the image (%llu bytes).",
                     join(path, fr.name).c_str(), (unsigned long long)fr.data_size);
            const u64 abs = data_off_ + fr.data_offset;
            files.push_back(RomfsFile{join(path, fr.name), abs, fr.data_size});
            total += fr.data_size;
            f = fr.next_sibling;
        }
        for (u32 s = d.first_child_dir; s != kNone;) {
            if (depth + 1 > kMaxDepth)
                fail(Fail::Input, "The RomFS is damaged: the folders are nested deeper than %u levels.", kMaxDepth);
            const DirRec sd = read_dir(s, false);
            mark_dir(s);
            const std::string sub = join(path, sd.name);
            dirs.push_back(sub);
            walk(sd, sub, depth + 1);
            s = sd.next_sibling;
        }
    }

    StoragePtr image_;
    u64 dir_off_, dir_size_, file_off_, file_size_, data_off_, img_size_;
    std::vector<bool> dir_seen_;
    std::vector<bool> file_seen_;
};

}  // namespace

std::unique_ptr<Romfs> Romfs::open(StoragePtr image) {
    const u64 total = image->size();
    if (total < kHeaderSize)
        fail(Fail::Input, "The RomFS is damaged: the image is only %llu bytes, too small to hold its header.",
             (unsigned long long)total);
    u8 h[kHeaderSize];
    image->read(0, h, sizeof h);
    if (rd64(h) != kHeaderSize)
        fail(Fail::Input, "The RomFS is damaged or of an unknown version: its header size is %llu, expected %llu.",
             (unsigned long long)rd64(h), (unsigned long long)kHeaderSize);

    struct Table { u64 off, size; const char* what; };
    const Table tables[] = {
        {rd64(h + 0x08), rd64(h + 0x10), "folder hash"},
        {rd64(h + 0x18), rd64(h + 0x20), "folder list"},
        {rd64(h + 0x28), rd64(h + 0x30), "file hash"},
        {rd64(h + 0x38), rd64(h + 0x40), "file list"},
    };
    for (const Table& t : tables)
        if (!fits(t.off, t.size, total))
            fail(Fail::Input, "The RomFS is damaged: its %s table runs past the end of the image.", t.what);

    const u64 dir_off = rd64(h + 0x18), dir_size = rd64(h + 0x20);
    const u64 file_off = rd64(h + 0x38), file_size = rd64(h + 0x40);
    const u64 data_off = rd64(h + 0x48);

    Walker w(image, dir_off, dir_size, file_off, file_size, data_off);
    w.run();

    std::unique_ptr<Romfs> out(new Romfs());
    out->files_ = std::move(w.files);
    out->dirs_ = std::move(w.dirs);
    out->total_ = w.total;
    out->image_ = std::move(image);
    return out;
}

StoragePtr Romfs::open_file(const RomfsFile& f) const { return make_sub_storage(image_, f.offset, f.size); }

}  // namespace mprs
