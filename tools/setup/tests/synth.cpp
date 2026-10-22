// SPDX-License-Identifier: GPL-3.0-or-later
// Builders for synthetic containers used by the tests. Standalone on purpose: no project crypto, linked into every test exe.
#include "synth.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace mprs_test {
namespace {

constexpr uint32_t kNone = 0xFFFFFFFF;

void put32(std::vector<uint8_t>& b, size_t pos, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[pos + i] = uint8_t(v >> (8 * i));
}
void put64(std::vector<uint8_t>& b, size_t pos, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[pos + i] = uint8_t(v >> (8 * i));
}
uint64_t align16(uint64_t x) { return (x + 15) & ~uint64_t(15); }
uint64_t pad4(uint64_t x) { return (x + 3) & ~uint64_t(3); }

uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// Splits "a/b/c" into {"a", "b", "c"}. Empty components are a bug in the test that uses this, so they throw.
std::vector<std::string> split_path(const std::string& p) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t slash = p.find('/', start);
        std::string part = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty()) throw std::invalid_argument("synth_romfs: empty path component in \"" + p + "\"");
        parts.push_back(part);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return parts;
}

struct DirNode {
    std::string name;
    int parent;
    std::vector<int> dirs;
    std::vector<int> files;
};

struct FileNode {
    std::string name;
    int parent;
    const std::vector<uint8_t>* data;
};

}  // namespace

std::array<uint8_t, 32> synth_sha256(const uint8_t* data, size_t len) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> m(data, data + len);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    const uint64_t bits = uint64_t(len) * 8;
    for (int i = 7; i >= 0; --i) m.push_back(uint8_t(bits >> (8 * i)));

    for (size_t blk = 0; blk < m.size(); blk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = &m[blk + 4 * i];
            w[i] = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = uint8_t(h[i] >> (24 - 8 * j));
    return out;
}

std::vector<uint8_t> synth_pfs0(const SynthFiles& files, uint32_t extra_string_pad) {
    std::string strtab;
    std::vector<uint32_t> name_offs;
    uint64_t data_total = 0;
    for (const auto& f : files) {
        name_offs.push_back(uint32_t(strtab.size()));
        strtab += f.first;
        strtab.push_back('\0');
        data_total += f.second.size();
    }
    strtab.append(extra_string_pad, '\0');

    const size_t n = files.size();
    const size_t header = 0x10 + 0x18 * n + strtab.size();
    std::vector<uint8_t> out(header + data_total, 0);
    std::memcpy(out.data(), "PFS0", 4);
    put32(out, 4, uint32_t(n));
    put32(out, 8, uint32_t(strtab.size()));

    uint64_t rel = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t pos = 0x10 + 0x18 * i;
        const auto& data = files[i].second;
        put64(out, pos, rel);
        put64(out, pos + 8, data.size());
        put32(out, pos + 16, name_offs[i]);
        if (!data.empty()) std::memcpy(out.data() + header + rel, data.data(), data.size());
        rel += data.size();
    }
    std::memcpy(out.data() + 0x10 + 0x18 * n, strtab.data(), strtab.size());
    return out;
}

std::vector<uint8_t> synth_hfs0(const SynthFiles& files) {
    std::string strtab;
    std::vector<uint32_t> name_offs;
    uint64_t data_total = 0;
    for (const auto& f : files) {
        name_offs.push_back(uint32_t(strtab.size()));
        strtab += f.first;
        strtab.push_back('\0');
        data_total += f.second.size();
    }

    const size_t n = files.size();
    const size_t header = 0x10 + 0x40 * n + strtab.size();
    std::vector<uint8_t> out(header + data_total, 0);
    std::memcpy(out.data(), "HFS0", 4);
    put32(out, 4, uint32_t(n));
    put32(out, 8, uint32_t(strtab.size()));

    uint64_t rel = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t pos = 0x10 + 0x40 * i;
        const auto& data = files[i].second;
        const uint32_t hashed = uint32_t(std::min<size_t>(data.size(), 0x200));
        put64(out, pos, rel);
        put64(out, pos + 8, data.size());
        put32(out, pos + 16, name_offs[i]);
        put32(out, pos + 20, hashed);
        const auto sha = synth_sha256(data.data(), hashed);
        std::memcpy(out.data() + pos + 0x20, sha.data(), sha.size());
        if (!data.empty()) std::memcpy(out.data() + header + rel, data.data(), data.size());
        rel += data.size();
    }
    std::memcpy(out.data() + 0x10 + 0x40 * n, strtab.data(), strtab.size());
    return out;
}

std::vector<uint8_t> synth_romfs(const SynthFiles& files) {
    std::vector<DirNode> dirs{{"", -1, {}, {}}};
    std::vector<FileNode> fnodes;
    for (const auto& [path, data] : files) {
        const std::vector<std::string> parts = split_path(path);
        int cur = 0;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            int found = -1;
            for (int c : dirs[cur].dirs)
                if (dirs[c].name == parts[i]) { found = c; break; }
            if (found < 0) {
                found = int(dirs.size());
                dirs.push_back(DirNode{parts[i], cur, {}, {}});
                dirs[cur].dirs.push_back(found);
            }
            cur = found;
        }
        fnodes.push_back(FileNode{parts.back(), cur, &data});
        dirs[cur].files.push_back(int(fnodes.size()) - 1);
    }

    const size_t nd = dirs.size(), nf = fnodes.size();
    std::vector<uint64_t> doff(nd), foff(nf), frel(nf);
    uint64_t dir_meta = 0, file_meta = 0;
    for (size_t i = 0; i < nd; ++i) {
        doff[i] = dir_meta;
        dir_meta += pad4(0x18 + dirs[i].name.size());
    }
    for (size_t i = 0; i < nf; ++i) {
        foff[i] = file_meta;
        file_meta += pad4(0x20 + fnodes[i].name.size());
    }

    const uint64_t dir_hash_size = 4 * std::max<size_t>(1, nd);
    const uint64_t file_hash_size = 4 * std::max<size_t>(1, nf);
    const uint64_t dir_hash_off = 0x50;
    const uint64_t dir_meta_off = dir_hash_off + dir_hash_size;
    const uint64_t file_hash_off = dir_meta_off + dir_meta;
    const uint64_t file_meta_off = file_hash_off + file_hash_size;
    const uint64_t file_data_off = align16(file_meta_off + file_meta);

    uint64_t cur = 0;
    for (size_t i = 0; i < nf; ++i) {
        frel[i] = cur;
        cur = align16(cur + fnodes[i].data->size());
    }
    std::vector<uint8_t> img(size_t(file_data_off + cur), 0);

    put64(img, 0x00, 0x50);
    put64(img, 0x08, dir_hash_off);
    put64(img, 0x10, dir_hash_size);
    put64(img, 0x18, dir_meta_off);
    put64(img, 0x20, dir_meta);
    put64(img, 0x28, file_hash_off);
    put64(img, 0x30, file_hash_size);
    put64(img, 0x38, file_meta_off);
    put64(img, 0x40, file_meta);
    put64(img, 0x48, file_data_off);
    std::fill(img.begin() + dir_hash_off, img.begin() + dir_hash_off + dir_hash_size, uint8_t(0xFF));
    std::fill(img.begin() + file_hash_off, img.begin() + file_hash_off + file_hash_size, uint8_t(0xFF));

    // Folder entries: links first (sibling links are filled in below), then name.
    for (size_t i = 0; i < nd; ++i) {
        const DirNode& d = dirs[i];
        const size_t p = size_t(dir_meta_off + doff[i]);
        put32(img, p + 0x00, d.parent < 0 ? 0 : uint32_t(doff[d.parent]));
        put32(img, p + 0x04, kNone);
        put32(img, p + 0x08, d.dirs.empty() ? kNone : uint32_t(doff[d.dirs[0]]));
        put32(img, p + 0x0C, d.files.empty() ? kNone : uint32_t(foff[d.files[0]]));
        put32(img, p + 0x10, kNone);
        put32(img, p + 0x14, uint32_t(d.name.size()));
        std::memcpy(img.data() + p + 0x18, d.name.data(), d.name.size());
    }
    for (const DirNode& d : dirs)
        for (size_t j = 0; j + 1 < d.dirs.size(); ++j)
            put32(img, size_t(dir_meta_off + doff[d.dirs[j]]) + 0x04, uint32_t(doff[d.dirs[j + 1]]));

    // File entries and their data.
    for (size_t i = 0; i < nf; ++i) {
        const FileNode& f = fnodes[i];
        const size_t p = size_t(file_meta_off + foff[i]);
        put32(img, p + 0x00, uint32_t(doff[f.parent]));
        put32(img, p + 0x04, kNone);
        put64(img, p + 0x08, frel[i]);
        put64(img, p + 0x10, f.data->size());
        put32(img, p + 0x18, kNone);
        put32(img, p + 0x1C, uint32_t(f.name.size()));
        std::memcpy(img.data() + p + 0x20, f.name.data(), f.name.size());
        if (!f.data->empty()) std::memcpy(img.data() + file_data_off + frel[i], f.data->data(), f.data->size());
    }
    for (const DirNode& d : dirs)
        for (size_t j = 0; j + 1 < d.files.size(); ++j)
            put32(img, size_t(file_meta_off + foff[d.files[j]]) + 0x04, uint32_t(foff[d.files[j + 1]]));

    return img;
}

}  // namespace mprs_test
