// SPDX-License-Identifier: GPL-3.0-or-later
// A game dump file (NSP / XCI, and NSZ / XCZ when zstd support is built in) as a list of NCA files plus the title keys of its tickets,
// and the step that finds the program NCA of the game setup is built for (game.h) in it.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "nca.h"
#include "nso.h"

namespace mprs {

struct ContainerFile {
    std::string name;      // e.g. "e66e21cdd2693f9c9d1e69dced3a8644.nca" (".cnmt.nca" for metadata)
    StoragePtr data;       // the plain NCA bytes (an NCZ is wrapped so that it reads like the NCA it came from)
};

struct Container {
    std::string kind;                    // "NSP", "XCI", "NSZ" or "XCZ"
    StoragePtr file;
    std::vector<ContainerFile> ncas;
    TicketKeys tickets;
};

// Detects the format by content (PFS0 magic = NSP/NSZ, "HEAD" at 0x100 = XCI/XCZ). Throws Error(Input) with a plain message for
// anything else ("This is not a Switch game dump").
std::unique_ptr<Container> open_container(const std::string& utf8_path);

// What was found in the dump for the given title.
struct DumpTitle {
    std::unique_ptr<Nca> program;        // the program NCA
    u64 title_id = 0;
    u32 title_version = 0;               // from the CNMT, 0 if it could not be read
    bool have_version = false;
    bool is_update = false;              // the dump only has patch content
    std::string other_title;             // set when a different game was found (16 hex digits)
};
// Opens every NCA header, picks the Program NCA of `title_id` (hex string, 16 digits). Throws Error(Input) with a message that says
// what the dump is instead (another game, an update-only dump, DLC only) when it is not there.
DumpTitle find_program(const Container& c, const KeySet& keys, const std::string& title_id);

// ---- several inputs: base game, update, DLC ----------------------------------------------------------------------------------

enum class MetaType : u8 { Application = 0x80, Patch = 0x81, AddOnContent = 0x82 };

// One content meta (the .cnmt file inside a Meta NCA).
struct ContentMeta {
    u64 title_id = 0;
    u32 version = 0;
    u8 type = 0;                         // MetaType value (others are kept but not used)
    u64 application_id = 0;              // Patch / AddOnContent: the game's title id (extended header), else 0
    struct Content { std::string id; u8 type = 0; };   // id: 32 lower-case hex digits; type: 0 meta, 1 program, 2 data, 3 control
    std::vector<Content> contents;
};
// Parses the bytes of a .cnmt file. Throws Error(Input) when it is too short.
ContentMeta parse_cnmt(const std::vector<u8>& bytes);

struct AocContent {
    u64 title_id = 0;
    u32 version = 0;
    std::unique_ptr<Nca> data;           // the Data / PublicData NCA (its RomFS holds the DLC files)
};

// What the inputs hold for one game: the base program, the newest update for it and every DLC of it.
struct DumpSet {
    u64 title_id = 0;
    std::unique_ptr<Nca> base;           // the Application's program NCA
    u32 base_version = 0;
    bool have_version = false;           // a CNMT for the base was found
    std::unique_ptr<Nca> patch;          // the newest Patch's program NCA, or null
    u32 patch_version = 0;
    std::unique_ptr<Nca> control;        // the control NCA of the update, else of the base (may be null)
    std::vector<AocContent> aoc;         // sorted by title id
    std::vector<u32> other_patches;      // versions of older updates that were found and not used
};
// Classifies every content of every input by its CNMT and picks the Application of `title_id`, the newest Patch (title id
// + 0x800) and the newest version of every AddOnContent (title id + 0x1000 and up, same upper bits). Tickets of all inputs are
// used for every input. Throws like find_program when the base game is not there.
DumpSet select_titles(const std::vector<const Container*>& inputs, const KeySet& keys, const std::string& title_id);

}  // namespace mprs
