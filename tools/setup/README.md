# First-run setup

The game ships without any Nintendo data. The setup tool builds the game folder from your own copy of the game. You run it once.
It is built for one game from that game's manifest (`games/<id>/game.json`), so the names below are the manifest's:

| Placeholder | Manifest field | Example |
|---|---|---|
| `<setup exe>`, `<setup cli>` | `files.setup`, `files.setup_cli` | `<id>_setup.exe`, `<id>_setup_cli.exe` |
| `<ini>` | `files.game_ini` | `<id>.ini` |
| `<shader pack>` | `files.shader_pack` | `shader_cache.<id>shc` |
| `<title id>` | `title_id` | the Switch title id |

The game's own README says how much disk space it needs and how long setup takes. A game that adds texture or shader steps to setup has
its own guide next to its hook (`games/<id>/setup/README.md`).

## What you need

- A dump of the game, a version listed in the manifest's `builds`, made from your own Switch: an `.nsp` (eShop) or `.xci` (cartridge)
  file. `.nsz` and `.xcz` work too.
- Only for a game whose supported version is an update: the update file too (an `.nsp` from the eShop, or the cartridge dump if
  the cartridge carries it). Give the base game and the update together, in any order; setup applies the newest update it is
  given and says which version it found and which versions it supports.
- Optional: DLC files (`.nsp`). Setup copies each DLC's files to `aoc\<title id>\` in the install folder.
- Your Switch keys (`prod.keys`), dumped from your own console with Lockpick_RCM. The tool only reads the file and never
  copies or uploads it. If you use an emulator (yuzu, Eden, Ryujinx and similar), the tool finds its keys by itself.
  If your eShop dump was bought on another console, put that console's title key in a `title.keys` file next to `prod.keys`.
- Free space on the drive you install to.
- The shader pack `<shader pack>`, in the same folder as `<setup exe>` (the release zip includes it), for a game that ships one. Setup
  stops with a message if it cannot find the pack. Use `--shader-pack` to point at another file or folder.

## Run it

1. Start `<setup exe>`.
2. Pick your dump file, or drag it into the window (pick or drag the update and DLC files with it, if you have them). Check the
   green line under it.
3. Pick the keys file if it was not found, and the install folder.
4. Press **Start setup**. You can press **Stop** at any time and run it again later; it carries on where it left off.

When it finishes, the install folder has everything the game needs. Start the game from that folder.

### Command line

```text
<setup cli> --input "game.nsp" --out D:\Game [--keys prod.keys] [--quality 3] [--threads 8]
<setup cli> --input base.xci --input update.nsp --input dlc.nsp --out D:\Game   # base game + update + DLC (or --input a,b,c)
<setup cli> --input dump.nsp --check            # only look at the dump, write nothing
<setup cli> --find-keys                         # list key files it can find
<setup cli> --input dump.nsp --out D:\Game --shader-pack D:\packs\<shader pack>   # pack from another place
<setup cli> --input dump.nsp --out D:\Game --no-shaders   # developer only: install without shaders
```

Exit codes: 0 done, 1 error, 2 the dump cannot be used or the shader pack is missing, 3 keys problem, 4 disk problem, 5 stopped (run it again to continue).

## What it makes

```
<install folder>\
  exefs\            the game program (copied from the dump unchanged)
  romfs\            the game's data files (with the update applied, when one was given)
  aoc\<title id>\   the files of each DLC that was given
  bc7_cache\        the game's textures converted to a format PC graphics cards read directly (a game with a texture hook)
  shader_cache\     translated shaders, copied from the shader pack
                    programs.bin lists the shader programs of your own game files; the game compiles them ahead from it
  <ini>             settings the game reads from this folder
  setup.log         what happened, every run
  setup_state.tsv   what is finished, so a second run only checks sizes
```

Running setup again on a finished folder only checks the files and takes a few seconds. `--verify-output` re-reads every
file and compares its hash.

## Texture quality

A game with a texture hook has its textures converted once, here, so the game does not do it while you play. The quality is stored in
`<ini>` (`bc7_quality`) and the game must use the same value, so leave it alone. For a different quality, run setup again with another
`--quality`. If a `bc7_cache` was made by an older converter (it carries `bc7_cache\encoder.txt` with the converter revision,
`kTextureEncoderRev` in `src/texcache.h`), setup throws it away and converts again; the cache file format is unchanged. The converter
keeps the alpha of opaque textures exactly at 255 (see `tools/textures/README.md`); the formats the cache entries use and the block
height rule are in the runtime's texture notes (`runtime/docs`) and the game's docs. `scripts\check_texcache.py` checks the entries as written against reference decodes.

## If something goes wrong

- **"The keys cannot open this dump"**: your keys file is older than the game. Dump the keys again from a console on recent
  firmware.
- **"This is an update"**: the update was given without the base game. Give both.
- **"This version of the game is not supported"**: the message names the version found and the supported ones. Give the
  supported update with the base game, or leave an unsupported update out.
- **"different game"**: use the dump of this game.
- **"The dump is damaged"**: the file was not copied completely or is corrupt. Copy it again, or dump the game again. The tool
  checks the dump's own hashes before it writes anything.
- **"Shader pack not found"**: put `<shader pack>` in the same folder as `<setup exe>`, or start setup with `--shader-pack` pointing at
  the file. If the install folder already has shaders from an earlier complete run, setup keeps them and does not need the pack.
- **"Not enough free space"**: free some space or pick another drive. Repeating setup is safe.
- **It stopped halfway** (power cut, crash): run it again. Finished files are kept.
- When you ask for help, send `setup.log` from the install folder. It contains no key data.

## Building it

```powershell
pwsh -File tools\textures\scripts\fetch_third_party.ps1   # once: astcenc, bc7enc_rdo, ISPC
pwsh -File tools\setup\scripts\fetch_zstd.ps1             # once: zstd (for NSZ/XCZ)
pwsh -File tools\setup\scripts\build.ps1 -Game games\<id> -Test   # CMake + Ninja + clang-cl, then the unit tests
```

You need Visual Studio Build Tools, LLVM (clang-cl), CMake and Ninja. The program is plain Win32 so it does not depend on
a graphics driver and works on a machine whose driver is not set up yet. Licences of the parts it uses are in `THIRD_PARTY.md`.

## Structure: base installer and game hook (developers)

The tool is built for one game (`-Game <games/id>`, CMake `NX_GAME_DIR`; its manifest `game.json` gives the title id, the supported
`builds[]` and the file names). The base installer does what every game needs: keys, containers, NCA and update patches, the
ExeFS, RomFS and DLC extraction, the shader pack, the state file and the ini. What only one game needs is its hook (`src/game_setup.h`,
`GameSetup`): `ini_lines`, `texture_cache` (the game's ASTC textures, fed to the generic BC7 cache writer in `src/texcache.*`),
`program_list` (the shader programs, written as `programs.bin`) and `disk_estimate`. The manifest's `setup_hook` names a CMake file that
defines the library `nxs_game_hook` (see `CMakeLists.txt`; the manifest names the file). A game without a hook is
extracted only: its texture and program stages are skipped.

The BC7 cache key, the `programs.bin` format and the shader pack layout are in `formats/nx_formats.h` (Python twin `nx_formats.py`),
which the runtime reads as well. Writers use the current magics `NXPRG002` and `NXSHC002` and the state head `nx_setup_state`; readers
also accept the v1 ones of the first release.
