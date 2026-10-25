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

}  // namespace mprs
