// SPDX-License-Identifier: GPL-3.0-or-later
#include "container.h"

#include <algorithm>
#include <cstring>

#include "log.h"
#include "ncz.h"

namespace mprs {
namespace {

u32 rd32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u64 rd64(const u8* p) { return u64(rd32(p)) | u64(rd32(p + 4)) << 32; }

bool ends_with(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suf[i]) return false;
    return true;
}

// Ticket: signature block (size by type), then the body (issuer 0x40, title key block 0x100, ...).
void parse_ticket(const IStorage& f, TicketKeys& out) {
    if (f.size() < 0x2C0 || f.size() > 0x10000) return;
    std::vector<u8> t = read_all(f, 0, static_cast<size_t>(f.size()));
    u32 sig_type = rd32(t.data());
    size_t body;
    switch (sig_type) {
        case 0x10000: case 0x10003: body = 0x240; break;   // RSA-4096
        case 0x10001: case 0x10004: body = 0x140; break;   // RSA-2048
        case 0x10002: case 0x10005: body = 0x80; break;    // ECDSA
        default: return;
    }
    if (t.size() < body + 0x180) return;
    const u8* b = t.data() + body;
    std::string rights = hex_encode(b + 0x160, 16);
    if (b[0x141] == 0) {
        Key128 k;
        std::memcpy(k.data(), b + 0x40, 16);
        out.common[rights] = k;
    } else {
        out.personalized.push_back(rights);
    }
}

void collect_pfs(const Pfs& p, Container& c, bool& saw_ncz) {
    for (const PfsEntry& e : p.entries()) {
        StoragePtr data = p.open_file(e);
        if (ends_with(e.name, ".nca")) {
            c.ncas.push_back({e.name, data});
        } else if (ends_with(e.name, ".ncz")) {
            saw_ncz = true;
            c.ncas.push_back({e.name.substr(0, e.name.size() - 4) + ".nca", open_ncz(data)});
        } else if (ends_with(e.name, ".tik")) {
            parse_ticket(*data, c.tickets);
        }
    }
}

}  // namespace

std::unique_ptr<Container> open_container(const std::string& path) {
    auto c = std::make_unique<Container>();
    c->file = open_file_storage(path);
    if (c->file->size() < 0x200) fail(Fail::Input, "This file is too small to be a Switch game dump.");
    u8 head[0x200];
    c->file->read(0, head, sizeof head);
    bool saw_ncz = false;
    if (std::memcmp(head, "PFS0", 4) == 0) {
        auto p = Pfs::open(c->file);
        collect_pfs(*p, *c, saw_ncz);
        c->kind = saw_ncz ? "NSZ" : "NSP";
    } else if (std::memcmp(head + 0x100, "HEAD", 4) == 0) {
        u64 root_off = rd64(head + 0x130);        // PartitionFsHeaderAddress (0x138 is the header's size)
        if (root_off < 0x200 || root_off >= c->file->size())
            fail(Fail::Input, "The cartridge dump (XCI) header is damaged.");
        auto root = Pfs::open(make_sub_storage(c->file, root_off, c->file->size() - root_off));
        bool any = false;
        for (const PfsEntry& e : root->entries()) {
            if (e.name != "secure" && e.name != "normal") continue;
            auto part = Pfs::open(root->open_file(e));
            collect_pfs(*part, *c, saw_ncz);
            any = true;
        }
        if (!any) fail(Fail::Input, "The cartridge dump (XCI) has no game partition (secure). It may be trimmed incorrectly.");
        c->kind = saw_ncz ? "XCZ" : "XCI";
    } else {
        fail(Fail::Input, "This file is not a Switch game dump (expected an NSP, XCI, NSZ or XCZ).");
    }
    if (c->ncas.empty()) fail(Fail::Input, "The dump contains no game archives (NCA files).");
    return c;
}

DumpTitle find_program(const Container& c, const KeySet& keys, const std::string& title_id) {
    u64 want = 0;
    for (char ch : title_id) {
        want <<= 4;
        want |= (ch >= '0' && ch <= '9') ? ch - '0' : (std::tolower(ch) - 'a' + 10);
    }
    DumpTitle r;
    int keyerrs = 0, opened = 0;
    std::string first_keyerr;
    std::unique_ptr<Nca> program;
    std::unique_ptr<Nca> meta;
    u64 other = 0;
    bool update_seen = false;
    for (const ContainerFile& f : c.ncas) {
        std::unique_ptr<Nca> n;
        try {
            n = Nca::open(f.data, keys, c.tickets);
        } catch (const Error& e) {
            if (e.kind == Fail::Keys) {
                ++keyerrs;
                if (first_keyerr.empty()) first_keyerr = e.what();
            } else {
                MPRS_LOGW("Skipping %s: %s", f.name.c_str(), e.what());
            }
            continue;
        }
        ++opened;
        if (n->content_type() == NcaContentType::Program) {
            if (n->program_id() == want && !program) program = std::move(n);
            else if (n->program_id() == want + 0x800) update_seen = true;
            else if (n->program_id() != want) other = n->program_id();
        } else if (n->content_type() == NcaContentType::Meta && n->program_id() == want && !meta) {
            meta = std::move(n);
        }
    }
    if (opened == 0 && keyerrs > 0) fail(Fail::Keys, "%s", first_keyerr.c_str());
    if (!program) {
        if (update_seen)
            fail(Fail::Input, "This is an update (patch) for the game, not the game itself. Setup needs the original v1.0.0 "
                              "dump (the base game), and updates are not supported.");
        if (other) {
            char b[32];
            std::snprintf(b, sizeof b, "%016llX", (unsigned long long)other);
            fail(Fail::Input, "This dump is a different game (title id %s). Setup needs Metroid Prime Remastered (%s).", b,
                 title_id.c_str());
        }
        fail(Fail::Input, "The dump does not contain the game program. It may be DLC or an incomplete dump.");
    }
    r.title_id = want;
    if (meta) {
        try {
            const NcaSection* s = meta->section(0);
            if (s && s->fs_type == 1) {
                auto p = meta->open_pfs0(0);
                for (const PfsEntry& e : p->entries()) {
                    if (!ends_with(e.name, ".cnmt")) continue;
                    u8 h[0x10];
                    if (e.size < 0x10) break;
                    p->open_file(e)->read(0, h, sizeof h);
                    if (rd64(h) == want) {
                        r.title_version = rd32(h + 8);
                        r.have_version = true;
                    }
                    break;
                }
            }
        } catch (const std::exception& ex) {
            MPRS_LOGW("Could not read the version from the dump: %s", ex.what());
        }
    }
    r.program = std::move(program);
    return r;
}

}  // namespace mprs
