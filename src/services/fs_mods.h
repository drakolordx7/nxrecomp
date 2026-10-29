// SPDX-License-Identifier: GPL-3.0-or-later
// Mod loading for the RomFS (LayeredFS style): a folder of mods next to the game exe whose files replace or add to the game's own
// RomFS files. The mods are discovered, ordered and indexed once at start-up into a ModOverlay; the two RomFS front ends of fsp-srv
// (the byte-addressed image in fs_romfs.h and the read-only IFileSystem volume in fs_volume.h) merge that overlay into the tree they
// serve. With no mods (or [mods] enabled = false) load_mods() returns null and nothing in those front ends changes.
//
// Folder layouts accepted (a "mod" is any folder that holds a romfs folder or romfs.bin; names are matched case-insensitively and the
// search goes down at most 8 levels, so a zip extracted with an extra wrapper folder is found):
//   mods/<mod>/romfs/...                                   Yuzu / Eden / Ryujinx style (the main layout)
//   mods/<mod>/romfs.bin                                   a packed RomFS image (Atmosphere, Ryujinx)
//   mods/<mod>/contents/<title id>/romfs/...               an Atmosphere "contents" tree inside a mod folder
//   mods/<mod>/atmosphere/contents/<title id>/romfs/...    an Atmosphere SD card tree extracted into a mod folder
//   mods/atmosphere/contents/<title id>/romfs/...          the same extracted straight into mods/
//   mods/contents/<title id>/<mod>/romfs/...               Ryujinx's mods folder
//   mods/<title id>/<mod>/romfs/...                        Eden's load folder
//   <mod>/romfs_ext/<path>.stub                            hides the game's <path> (a file or a whole folder); Yuzu / Eden
// A <title id> folder is used only when it is the running title's (16 hex digits, any case); other titles' folders are skipped.
// Priorities: a file in an enabled mod replaces the game's file of the same path, and between mods the one listed higher in mods.ini
// wins. Inside one mod the loose romfs folder wins over romfs.bin, as in Atmosphere (loose files over the SD card romfs.bin over the base)
// and Ryujinx (loose files first, then romfs.bin, then the base). Paths are compared case-sensitively, as Horizon does; a mod file that
// differs from a game file only in case is added as a separate file and a warning says so.
//
// Sources: Atmosphere changelog 0.10.x ("loose files are preferred to content in the SD card romfs.bin image, and files from the SD
// card image are preferred to those in the base"; stratosphere/ams_mitm fsmitm_romfs.cpp, where a path already added is not replaced);
// Ryujinx src/Ryujinx.HLE/HOS/ModLoader.cs (RomfsDir "romfs", RomfsContainer "romfs.bin", contents/<title id> search, case-insensitive
// directory names, ExeFS/exefs_patches folders); Eden/yuzu src/core/file_sys/patch_manager.cpp (ApplyLayeredFS: romfs, romfs_ext layers,
// caseless folder lookup, mods sorted by name with the first one winning) and fsmitm_romfsbuild.cpp (`<name>.stub` in romfs_ext hides a
// base file or folder). The `.stub` marker is a Yuzu/Eden convention: Atmosphere rejected it (Atmosphere-NX/Atmosphere issue 1923),
// and Ryujinx uses `.stub` only for ExeFS NSOs.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nxrt::services::fs {

constexpr size_t kRomfsMaxName = 255;     // bytes of UTF-8 in one RomFS name
constexpr size_t kRomfsMaxPath = 0x300;   // SDK FS_MAX_PATH (0x301 with the terminator); the path counts its leading '/'

// What load_mods() needs; mods_config_from_runtime() fills it from [mods], the exe folder and the running title.
struct ModsConfig {
    bool enabled = true;     // [mods] enabled
    std::string mods_dir;    // UTF-8, the folder holding the mods
    std::string ini_path;    // mods.ini (order and enabled flags); empty: not read and not written
    uint64_t title_id = 0;   // program id used to pick <title id> folders; 0 accepts every one
};
ModsConfig mods_config_from_runtime();

// Where the bytes of one mod file are: a host file, or a member of a packed romfs.bin (offset/size inside that file).
struct ModSource {
    std::wstring host;       // ready to open (absolute, \\?\ prefix when long)
    uint64_t offset = 0;
    uint64_t size = 0;
    std::shared_ptr<const std::vector<uint8_t>> mem;   // generated content (ModsIndexEdit::replace_file): host and offset are unused
};

// The merged result of all enabled mods: which paths they provide, which game paths they hide. Immutable after load_mods().
// Paths are RomFS paths: "/", "/a", "/a/b.bin" (UTF-8, case-sensitive).
class ModOverlay {
public:
    struct Child {
        std::string name;
        bool is_dir = false;
    };
    // One line per mod in the order of mods.ini (highest priority first), for the log and the tests.
    struct ModInfo {
        std::string name;
        bool enabled = false;
        std::string folder;                 // the folder that was found (the mod's first root)
        size_t files = 0;                   // files found in the mod
        size_t overrides = 0, additions = 0;   // files served from this mod: replacing a game file / new ones
        size_t lost = 0;                    // found but beaten by a higher-priority mod or hidden by one
        size_t hides = 0;                   // game entries its .stub markers hide
        std::vector<std::string> unsupported;   // exefs, exefs_patches, cheats, ... (ignored)
    };

    bool empty() const { return files_.empty() && hidden_.empty(); }
    size_t file_count() const { return files_.size(); }

    const ModSource* find_file(const std::string& path) const;
    bool is_dir(const std::string& path) const { return dirs_.count(path) != 0; }
    // For a folder the mods provide: whether the game has a folder of exactly that name (case-sensitive) that is not hidden. Folders
    // that no mod touches are not asked about; true for them.
    bool base_dir_exists(const std::string& path) const {
        auto it = dirs_.find(path);
        return it == dirs_.end() || it->second.base;
    }
    // A folder some mod provides: a host folder it exists in (only for times), or null.
    const std::wstring* dir_host(const std::string& path) const;
    const std::vector<Child>* children(const std::string& dir) const;   // the mods' entries of a folder, unsorted
    // True when `path` or one of its parents is hidden from the game's own tree (a mod entry at that path can still exist).
    bool hidden(const std::string& path) const;
    const std::vector<std::string>& dirs_sorted() const { return dir_list_; }      // every folder the mods provide, "/" first
    const std::vector<std::string>& hidden_paths() const { return hidden_list_; }
    const std::vector<ModInfo>& mods() const { return infos_; }

private:
    friend class OverlayBuilder;
    friend class ModsIndexEdit;
    friend std::shared_ptr<const ModOverlay> load_mods(const ModsConfig& cfg, const std::string& base_romfs_dir);
    struct FileRec {
        ModSource src;
        uint32_t mod = 0;
    };
    struct DirRec {
        bool base = true;       // the game has this folder (set by the comparison with the game's tree)
        std::wstring host;
        std::vector<Child> kids;
    };
    std::unordered_map<std::string, FileRec> files_;
    std::unordered_map<std::string, DirRec> dirs_;
    std::unordered_map<std::string, uint32_t> hidden_;   // hidden game path -> index of the mod whose .stub hides it
    std::vector<std::string> dir_list_, hidden_list_;
    std::vector<ModInfo> infos_;
};

// ---- the hook for game modules -----------------------------------------------------------------------------------------------------
// Some games keep data that must agree with the files that are served; ACNH's resource size table (System/Resource/ResourceSizeTable.
// srsizetable) lists the size of every resource, and a modded file of another size without a matching entry can crash the game. The
// base knows nothing about such tables. A game module registers a hook (set_mods_index_hook, from its own registration code); load_mods
// calls it once, after the merged index is complete and before it is served, with the list of files the mods change (old and new
// size) and the means to read the merged view and to put a generated file in place of a served one (in memory, never on disk).
// The hook is not called when no mod provides anything, so the no-mods path is untouched.
struct ModFileChange {
    std::string path;                    // "/System/Resource/ResourceSizeTable.srsizetable"
    bool replaces_game_file = false;     // false: the mods add this file; the game has none
    uint64_t old_size = 0;               // the game's file size (0 when it adds)
    uint64_t new_size = 0;               // the size that is served
    std::string mod;                     // the mod the served file comes from
};

class OverlayBuilder;

class ModsIndexEdit {
public:
    // Every file the mods serve, sorted by path.
    const std::vector<ModFileChange>& changes() const { return changes_; }
    // The game paths the mods hide (files or folders), sorted.
    const std::vector<std::string>& hidden() const { return hidden_; }
    // The content `path` has in the merged view (a mod's file if one provides it, else the game's); false when absent or hidden.
    bool read_file(const std::string& path, std::vector<uint8_t>* out) const;
    // Serves `data` at `path` instead of whatever is there (a mod's file, the game's, or nothing). False for a path that is not a valid
    // RomFS file path or names a folder. Counted in replaced().
    bool replace_file(const std::string& path, std::vector<uint8_t> data);
    size_t replaced() const { return replaced_; }

private:
    friend std::shared_ptr<const ModOverlay> load_mods(const ModsConfig& cfg, const std::string& base_romfs_dir);
    ModsIndexEdit(ModOverlay* o, OverlayBuilder* b, std::string base_root, std::vector<ModFileChange> changes);
    ModOverlay* o_;
    OverlayBuilder* b_;
    std::string base_root_;
    std::vector<ModFileChange> changes_;
    std::vector<std::string> hidden_;
    size_t replaced_ = 0;
};

using ModsIndexHook = std::function<void(ModsIndexEdit&)>;
// Registers the hook (one per process; an empty function removes it). Call before the game first opens its RomFS (the mods are loaded then).
void set_mods_index_hook(ModsIndexHook hook);

// Finds and loads the mods. `base_romfs_dir` is the game's extracted RomFS folder: it is listed (one directory listing per folder the
// mods touch, nothing more) to tell replaced files from added ones and to catch case and type mismatches. Returns null when mods are
// disabled, the folder is missing, or no enabled mod provides anything. Logs one line per mod (tag "mods").
std::shared_ptr<const ModOverlay> load_mods(const ModsConfig& cfg, const std::string& base_romfs_dir);

// Reads a packed RomFS image (romfs.bin) and lists its files with their absolute offsets in the file. False with *err set when the
// header or the tables are not those of a RomFS image. Entries that point outside the file are skipped (counted in *skipped).
struct PackedEntry {
    std::string path;       // "/a/b.bin"
    uint64_t offset = 0;    // in the image file
    uint64_t size = 0;
};
bool read_packed_romfs(const std::wstring& file, std::vector<PackedEntry>* files, std::vector<std::string>* dirs, size_t* skipped,
                       std::string* err);

}  // namespace nxrt::services::fs
