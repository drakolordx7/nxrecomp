# Mods

You can replace or add game files with mods, the way Atmosphère LayeredFS, Ryujinx and Eden do on their side. Put a mod in the `mods` folder next to the game exe and start the game. The game's own files are never touched: the mods are laid over them while the game runs, and removing a mod puts everything back.

## Installing a mod

1. Extract the mod (mods come as .zip, .7z or .rar archives; the game does not open archives).
2. Put the extracted folder in the `mods` folder next to the game exe, one folder per mod:

   ```
   mods/
     My Cool Mod/
       romfs/
         Some/Folder/file.bin
   ```

3. Start the game. The first start writes the list of mods to `mods.ini` next to the exe, with every mod switched on.

The `romfs` folder holds files at the same paths as in the game's own RomFS: `romfs/Some/Folder/file.bin` replaces the game's `Some/Folder/file.bin`. A file the game does not have is added. Folders merge.

### Folder layouts that work

All of these are found, so you can usually extract a mod as it comes:

| Layout | Comes from |
|---|---|
| `mods/<mod>/romfs/...` | Yuzu, Eden, Ryujinx (the usual one) |
| `mods/<mod>/atmosphere/contents/<title id>/romfs/...` | An Atmosphère SD card tree extracted into a folder |
| `mods/<mod>/contents/<title id>/romfs/...` | Atmosphère `contents` folder |
| `mods/atmosphere/contents/<title id>/romfs/...` | A mod zip extracted straight into `mods` |
| `mods/contents/<title id>/<mod>/romfs/...` | Ryujinx's mods folder |
| `mods/<title id>/<mod>/romfs/...` | Eden's `load` folder |
| `mods/<mod>/romfs.bin` | A packed RomFS image (Atmosphère, Ryujinx) |

Other things you can run into:

- A zip that was extracted with an extra folder around the mod (`mods/Pack/My Mod/romfs/...`) works; the mod is called `Pack/My Mod`.
- Folder names are matched without regard to case (`RomFS`, `ROMFS`), and so is the `<title id>`. Only the folder for this game's title id is used; mods for other games are skipped, and the log says so.
- The mod's own files must match the game's file names exactly, including capital letters (see "Paths are case-sensitive" below).
- A `romfs.bin` and a `romfs` folder in the same mod can both be there: the loose files win over the files inside the `romfs.bin`.
- `mods/<mod>/romfs_ext/<path>.stub` (an empty file) hides the game's file or folder at `<path>`. This is the Yuzu and Eden way of removing a file from a game. Atmosphère has no such feature.

## Switching mods off and choosing which wins

`mods.ini` next to the exe lists the mods, one per line, highest priority first:

```ini
; A mod listed higher wins over a mod listed below it.
[order]
Better Textures = on
My Cool Mod = on
Old Experiment = off
```

- `= off` (or `false`, `no`, `0`) switches a mod off without deleting it.
- When two mods have the same file, the one listed higher is used. Every mod wins over the game's own files.
- A mod you add later is put at the bottom of the list, switched on. Move its line up to give it priority. The game keeps your comments and the order you set. Entries for mods you moved away stay in the file, so a mod comes back in the same place.
- Mod names are the folder names. A mod inside a wrapper folder is written `Wrapper/Mod`. Capital letters and `\` or `/` do not matter.

To switch every mod off at once, set `enabled = false` under `[mods]` in `nxrt.ini` (see `CONFIG.md`). The same section can point to another mods folder (`dir`) or another list file (`ini`).

## What works

- Replacing, adding and hiding RomFS files, through everything the game uses to read them: opening files, reading, file sizes, listing folders and asking what a path is.
- Many mods together, with priorities and per-mod on/off.
- Packed `romfs.bin` images.
- Large mods: the list of the mods' files is built once at start-up, in a hash map, so opening thousands of files costs nothing extra. With no mods nothing is read or changed.

## What does not work yet

- **Code patches.** `exefs` folders, `exefs_patches`, IPS and pchtxt patches, and `exefs.nsp` change the game's program code on a console. This game is a recompilation: there is no code to patch at run time. A mod that has these is loaded for its RomFS files only, and the log says once which mods have code parts that are ignored. Code mods are planned separately.
- **Cheats** (`cheats` folders) are not supported in this version. Planned separately.
- **IPS patches of RomFS files** (`.ips` files in `romfs_ext`) are not applied.
- **Mod manager.** Order and on/off are edited in `mods.ini` by hand for now.
- **Games' own size tables.** Some games keep a table of the size of each resource (Animal Crossing: New Horizons has `System/Resource/ResourceSizeTable.srsizetable`, the RSTB). When a mod file has another size than the original, the table must be fixed or the game can crash or leave objects out. That is handled per game, not here: the mod loader gives the game module a hook that lists every changed file with its old and new size and lets it serve a regenerated table from memory (see the end of this page). Until the game module does so, modders have to patch such tables as they do on a console.

## Paths are case-sensitive

The game's file system tells `Font/a.bin` and `font/a.bin` apart, and Windows does not. A mod file whose name differs from a game file only in capitals is **not** a replacement: it is added as a separate file that the game never asks for, and the log shows a warning like

```
mods: mod 'My Mod': '/font' differs from the game's '/Font' only in case; ... (rename it to match)
```

Rename the file or folder so it matches the game's spelling.

## If a mod does not load

Open `nxrt.log` next to the exe and search for lines with `mods:`. There is one line per mod:

```
mods: mod 'My Cool Mod': 12 files (10 replace game files, 2 new), hides 1 game entry
mods: 2 mods on, 1 off: 12 RomFS files served from mods (10 replace game files, 2 new)
```

and clear warnings for what is wrong, for example:

| Log line says | What to do |
|---|---|
| `folder 'X' holds no RomFS content` | The mod has no `romfs` folder (or `romfs.bin`). Check the folder layout above. |
| `'X.zip' is an archive` | Extract it into its own folder in `mods`. |
| `'romfs' sits directly in the mods folder` | Put the `romfs` folder inside a folder for the mod. |
| `skipping '...': it is for title ...` | The mod is for another game. |
| `... differs from the game's ... only in case` | Rename the file as the warning says. |
| `... is a file in the mod but a folder in the game` (or the other way round) | The mod's layout does not match the game; that entry is ignored. |
| `conflict: 'A' (higher in mods.ini) wins over 'B' on N files` | Two mods change the same files. Reorder them in `mods.ini` if the other should win. |
| `... is not usable: it ...` (for a `romfs.bin`) | The packed image is damaged or is not a RomFS image. |
| `code patches, cheats and file patches are NOT supported` | See "What does not work yet". The RomFS files of those mods still load. |
| `mods.ini cannot be written` | The game folder is read-only. The mods are still used; edit the order elsewhere (`[mods] ini`). |

Set `[log] level = debug` to also get one line per file saying whether the mod replaces or adds it.

## For game modules

The base keeps no knowledge of any game's tables. A game module can register a hook before the game first opens its RomFS (the mods are loaded then):

```cpp
nxrt::services::fs::set_mods_index_hook([](nxrt::services::fs::ModsIndexEdit& edit) {
    for (const auto& c : edit.changes())      // path, replaces_game_file, old_size, new_size, mod
        ...;
    std::vector<uint8_t> table;
    if (edit.read_file("/System/Resource/ResourceSizeTable.srsizetable", &table)) {   // the merged view
        ...;                                                                           // fix up the entries
        edit.replace_file("/System/Resource/ResourceSizeTable.srsizetable", std::move(table));
    }
});
```

The hook runs once, after the merged list of files is complete and before it is served, and only when at least one mod provides something. Generated files live in memory; nothing is written to disk. See `src/services/fs_mods.h`.
