// SPDX-License-Identifier: GPL-3.0-or-later
#include "container.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>

#include "game.h"
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

ContentMeta parse_cnmt(const std::vector<u8>& b) {
    if (b.size() < 0x20) fail(Fail::Input, "A content list (CNMT) in the dump is too short.");
    ContentMeta m;
    m.title_id = rd64(b.data());
    m.version = rd32(b.data() + 8);
    m.type = b[0xC];
    const size_t ext = size_t(b[0xE]) | size_t(b[0xF]) << 8;
    const size_t count = size_t(b[0x10]) | size_t(b[0x11]) << 8;
    if ((m.type == u8(MetaType::Patch) || m.type == u8(MetaType::AddOnContent)) && ext >= 8 && b.size() >= 0x28)
        m.application_id = rd64(b.data() + 0x20);
    const size_t rec = 0x20 + ext;
    for (size_t i = 0; i < count && rec + 0x38 * (i + 1) <= b.size(); ++i) {
        const u8* r = b.data() + rec + 0x38 * i;
        m.contents.push_back({hex_encode(r + 0x20, 16), r[0x36]});
    }
    return m;
}

namespace {

u64 parse_title_id(const std::string& title_id) {
    u64 want = 0;
    for (char ch : title_id) {
        want <<= 4;
        want |= (ch >= '0' && ch <= '9') ? ch - '0' : (std::tolower(ch) - 'a' + 10);
    }
    return want;
}

std::string tid_hex(u64 t) {
    char b[32];
    std::snprintf(b, sizeof b, "%016llX", (unsigned long long)t);
    return b;
}

// The content id an NCA's file name carries ("<32 hex>.nca" / "<32 hex>.cnmt.nca"), lower case, or "".
std::string content_id_of(const std::string& name) {
    std::string id = name.substr(0, std::min(name.find('.'), name.size()));
    for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos) return "";
    return id;
}

struct Item {
    size_t input = 0;
    std::string id;                      // content id from the file name, or ""
    std::unique_ptr<Nca> nca;
};

struct Meta {
    size_t input = 0;
    ContentMeta m;
};

}  // namespace

DumpSet select_titles(const std::vector<const Container*>& inputs, const KeySet& keys, const std::string& title_id) {
    const u64 want = parse_title_id(title_id);
    const u64 patch_id = want + 0x800, aoc_base = want + 0x1000;
    TicketKeys tickets;
    for (const Container* c : inputs) {
        for (auto& [k, v] : c->tickets.common) tickets.common[k] = v;
        for (auto& p : c->tickets.personalized) tickets.personalized.push_back(p);
    }
    std::vector<Item> items;
    std::map<std::string, std::string> key_failed;   // content id -> the key error
    int keyerrs = 0, opened = 0;
    std::string first_keyerr;
    for (size_t ci = 0; ci < inputs.size(); ++ci) {
        for (const ContainerFile& f : inputs[ci]->ncas) {
            std::unique_ptr<Nca> n;
            try {
                n = Nca::open(f.data, keys, tickets);
            } catch (const Error& e) {
                if (e.kind == Fail::Keys) {
                    ++keyerrs;
                    if (first_keyerr.empty()) first_keyerr = e.what();
                    if (std::string id = content_id_of(f.name); !id.empty()) key_failed[id] = e.what();
                } else {
                    MPRS_LOGW("Skipping %s: %s", f.name.c_str(), e.what());
                }
                continue;
            }
            ++opened;
            items.push_back({ci, content_id_of(f.name), std::move(n)});
        }
    }
    if (opened == 0 && keyerrs > 0) fail(Fail::Keys, "%s", first_keyerr.c_str());

    std::vector<Meta> metas;
    for (const Item& it : items) {
        if (it.nca->content_type() != NcaContentType::Meta) continue;
        try {
            const NcaSection* s = it.nca->section(0);
            if (!s || s->fs_type != 1) continue;
            auto p = it.nca->open_pfs0(0);
            for (const PfsEntry& e : p->entries()) {
                if (!ends_with(e.name, ".cnmt")) continue;
                const size_t n = static_cast<size_t>(std::min<u64>(e.size, 1u << 20));
                metas.push_back({it.input, parse_cnmt(read_all(*p->open_file(e), 0, n))});
                break;
            }
        } catch (const std::exception& ex) {
            MPRS_LOGW("Could not read a content list in the dump: %s", ex.what());
        }
    }
    // Newest first, so the first match of a title wins.
    std::stable_sort(metas.begin(), metas.end(), [](const Meta& a, const Meta& b) { return a.m.version > b.m.version; });

    DumpSet r;
    r.title_id = want;
    r.input_used.assign(inputs.size(), false);
    // The NCA a meta lists for a content type (it must also fit); else (no list, or renamed files) the one NCA in the same input
    // its header matches. A listed content whose header the keys could not open is a keys problem.
    auto take = [&](const Meta& mt, u8 ctype, const char* what, const std::function<bool(const Nca&)>& fits) -> std::unique_ptr<Nca> {
        for (const auto& c : mt.m.contents) {
            if (c.type != ctype) continue;
            for (Item& it : items)
                if (it.nca && it.id == c.id && fits(*it.nca)) {
                    r.input_used[it.input] = true;
                    r.input_used[mt.input] = true;
                    return std::move(it.nca);
                }
            if (auto k = key_failed.find(c.id); k != key_failed.end())
                fail(Fail::Keys, "The keys cannot open the %s (%s.nca): %s", what, c.id.c_str(), k->second.c_str());
        }
        Item* pick = nullptr;
        for (Item& it : items)
            if (it.nca && it.input == mt.input && fits(*it.nca)) {
                if (pick) return nullptr;   // more than one: cannot tell
                pick = &it;
            }
        if (!pick) return nullptr;
        r.input_used[pick->input] = true;
        return std::move(pick->nca);
    };
    // An update's program and control carry the game's title id, or (some dumps) the update's.
    auto is_program = [&](const Nca& n, bool patch) {
        if (n.content_type() != NcaContentType::Program) return false;
        if (n.program_id() != want && !(patch && n.program_id() == patch_id)) return false;
        const int rs = n.romfs_section();
        return rs < 0 || n.section(rs)->is_patch() == patch;
    };
    auto is_control = [&](const Nca& n, bool patch) {
        return n.content_type() == NcaContentType::Control && (n.program_id() == want || (patch && n.program_id() == patch_id));
    };
    auto for_game = [&](const ContentMeta& m) { return m.application_id == 0 || m.application_id == want; };

    for (const Meta& mt : metas) {
        if (mt.m.type != u8(MetaType::Application) || mt.m.title_id != want) continue;
        r.base = take(mt, 1, "base game program", [&](const Nca& n) { return is_program(n, false); });
        if (!r.base) continue;
        r.base_version = mt.m.version;
        r.have_version = true;
        r.control = take(mt, 3, "base game control data", [&](const Nca& n) { return is_control(n, false); });
        break;
    }
    if (!r.base) {   // no content list: the first base program of the game
        for (Item& it : items)
            if (it.nca && is_program(*it.nca, false) && it.nca->program_id() == want) {
                r.input_used[it.input] = true;
                r.base = std::move(it.nca);
                break;
            }
    }
    if (!r.base) {
        bool update_seen = false;
        u64 other = 0;
        for (const Meta& mt : metas)
            if (mt.m.type == u8(MetaType::Patch) && mt.m.title_id == patch_id) update_seen = true;
        for (const Item& it : items) {
            if (!it.nca || it.nca->content_type() != NcaContentType::Program) continue;
            if (it.nca->program_id() == patch_id || it.nca->program_id() == want) update_seen = true;
            else other = it.nca->program_id();
        }
        if (update_seen) {
            if (!game_lists_builds())
                fail(Fail::Input, "This is an update (patch) for the game, not the game itself. Setup needs the original v%s "
                                  "dump (the base game), and updates are not supported.", game().version);
            fail(Fail::Input, "This is an update (patch) for the game, not the game itself. Setup needs the base game dump too: "
                              "give the base game and the update together.");
        }
        if (other)
            fail(Fail::Input, "This dump is a different game (title id %s). Setup needs %s (%s).", tid_hex(other).c_str(),
                 game().title, title_id.c_str());
        fail(Fail::Input, "The dump does not contain the game program. It may be DLC or an incomplete dump.");
    }

    for (const Meta& mt : metas) {
        if (mt.m.type != u8(MetaType::Patch) || mt.m.title_id != patch_id || !for_game(mt.m)) continue;
        if (r.patch) {
            if (mt.m.version != r.patch_version) r.other_patches.push_back(mt.m.version);
            continue;
        }
        r.patch = take(mt, 1, "update's program", [&](const Nca& n) { return is_program(n, true); });
        if (!r.patch) {
            r.skipped.push_back("The update v" + std::to_string(mt.m.version) + " has no readable program; it was not used.");
            MPRS_LOGW("%s", r.skipped.back().c_str());
            continue;
        }
        r.patch_version = mt.m.version;
        if (auto c = take(mt, 3, "update's control data", [&](const Nca& n) { return is_control(n, true); })) r.control = std::move(c);
    }

    for (const Meta& mt : metas) {
        const u64 t = mt.m.title_id;
        if (mt.m.type != u8(MetaType::AddOnContent) || (t & ~u64(0xFFF)) != aoc_base || !for_game(mt.m)) continue;
        if (std::any_of(r.aoc.begin(), r.aoc.end(), [&](const AocContent& a) { return a.title_id == t; })) continue;
        auto is_data = [t](const Nca& n) {
            return (n.content_type() == NcaContentType::Data || n.content_type() == NcaContentType::PublicData) && n.program_id() == t;
        };
        const std::string what = "DLC " + tid_hex(t);
        std::unique_ptr<Nca> d = take(mt, 2, what.c_str(), is_data);
        if (!d) {
            r.skipped.push_back("DLC " + tid_hex(t) + " v" + std::to_string(mt.m.version) + " has no readable data; it was not used.");
            MPRS_LOGW("%s", r.skipped.back().c_str());
            continue;
        }
        r.aoc.push_back({t, mt.m.version, std::move(d)});
    }
    std::sort(r.aoc.begin(), r.aoc.end(), [](const AocContent& a, const AocContent& b) { return a.title_id < b.title_id; });
    // An input whose content list names this game belongs to it (a copy of a DLC or an older update counts too).
    for (const Meta& mt : metas) {
        const u64 t = mt.m.title_id;
        if (t == want || t == patch_id || ((t & ~u64(0xFFF)) == aoc_base && for_game(mt.m))) r.input_used[mt.input] = true;
    }
    return r;
}

DumpTitle find_program(const Container& c, const KeySet& keys, const std::string& title_id) {
    DumpSet s = select_titles({&c}, keys, title_id);
    DumpTitle r;
    r.program = std::move(s.base);
    r.title_id = s.title_id;
    r.title_version = s.base_version;
    r.have_version = s.have_version;
    return r;
}

}  // namespace mprs
