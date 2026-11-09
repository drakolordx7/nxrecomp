# Metroid Prime Remastered PC: first-run setup

The game ships without any Nintendo data. This tool builds the game folder from your own copy of the game.
You run it once. It needs about 13 GB of free disk space and takes about 5 minutes on a 16-core PC.

## What you need

- A dump of the game, version 1.0.0, made from your own Switch: an `.nsp` (eShop) or `.xci` (cartridge) file.
  `.nsz` and `.xcz` work too. The base game is title id `010012101468C000`.
- Only for a game whose supported version is an update: the update file too (an `.nsp` from the eShop, or the cartridge dump if
  the cartridge carries it). Give the base game and the update together, in any order; setup applies the newest update it is
  given and says which version it found and which versions it supports. Metroid Prime Remastered needs no update.
- Optional: DLC files (`.nsp`). Setup copies each DLC's files to `aoc\<title id>\` in the install folder.
- Your Switch keys (`prod.keys`), dumped from your own console with Lockpick_RCM. The tool only reads the file and never
  copies or uploads it. If you use an emulator (yuzu, Eden, Ryujinx and similar), the tool finds its keys by itself.
  If your eShop dump was bought on another console, put that console's title key in a `title.keys` file next to `prod.keys`.
- 13 GB of free space on the drive you install to.
- The shader pack `shader_cache.mprshc`, in the same folder as `mpr_setup.exe` (the release zip includes it). Setup stops with a
  message if it cannot find the pack, because the game draws nothing without it. Use `--shader-pack` to point at another file or folder.

## Run it

1. Start `mpr_setup.exe`.
2. Pick your dump file, or drag it into the window (pick or drag the update and DLC files with it, if you have them). Check the
   green line under it.
3. Pick the keys file if it was not found, and the install folder.
4. Press **Start setup**. You can press **Stop** at any time and run it again later; it carries on where it left off.

When it finishes, the install folder has everything the game needs. Start the game from that folder.

### Command line

```text
mpr_setup_cli --input "Metroid Prime Remastered.nsp" --out D:\MPR [--keys prod.keys] [--quality 3] [--threads 8]
mpr_setup_cli --input base.xci --input update.nsp --input dlc.nsp --out D:\Game   # base game + update + DLC (or --input a,b,c)
mpr_setup_cli --input dump.nsp --check            # only look at the dump, write nothing
mpr_setup_cli --find-keys                         # list key files it can find
mpr_setup_cli --input dump.nsp --out D:\MPR --shader-pack D:\packs\shader_cache.mprshc   # pack from another place
mpr_setup_cli --input dump.nsp --out D:\MPR --no-shaders   # developer only: install without shaders; the game draws nothing
```

Exit codes: 0 done, 1 error, 2 the dump cannot be used or the shader pack is missing, 3 keys problem, 4 disk problem, 5 stopped (run it again to continue).

## What it makes

```
<install folder>\
  exefs\            the game program (copied from the dump unchanged)
  romfs\            the game's data files (with the update applied, when one was given)
  aoc\<title id>\   the files of each DLC that was given
  bc7_cache\        the game's textures converted to a format PC graphics cards read directly (about 4.4 GB)
  shader_cache\     translated shaders, copied from the shader pack
                    programs.bin lists the shader programs of your own game files; the game compiles them ahead from it (see "Prepare shaders" in the launcher)
  mpr.ini           settings the game reads from this folder
  setup.log         what happened, every run
  setup_state.tsv   what is finished, so a second run only checks sizes
```

Running setup again on a finished folder only checks the files and takes a few seconds. `--verify-output` re-reads every
file and compares its hash.

## Texture quality

Textures are converted once, here, so the game does not do it while you play. The quality is stored in `mpr.ini`
(`bc7_quality`) and the game must use the same value, so leave it alone. For a different quality, run setup again with
another `--quality`. The old textures stay on disk until you delete `bc7_cache`.

| Quality | Time on a 16-core PC | Notes |
|---|---|---|
| 3 (default) | about 5 minutes in all (textures 250 s of 295 s, measured) | the value the game is tuned for |
| 5 | about 15 minutes (estimated from the converter's speed ratio) | 0.2 to 1 dB better on the noisiest textures |

The converter keeps the alpha of opaque textures exactly at 255 (see `tools/textures/README.md`). Two kinds of texture are
not stored as BC7: two-channel normal maps (a linear texture that is fully opaque with a blue channel of at most 2) are stored as
BC5, which keeps red and green in two 8-value ramps per block and is sampled as (R, G, 0, 1); and a texture BC7 cannot hold
(its BC7 result is under 28 dB, today only the white-noise lookup texture at 19 dB) is stored as plain RGBA8. The file name and
key do not change; the entry's header names the format and the game builds its image in that format (`runtime/docs/S4f.md`).
The block height of small textures follows the console's rule (the runtime's `console_block_height`), which changes the key and
the untiling of the textures where it differs from the smallest covering height. If a `bc7_cache` was made by an older converter (it carries
`bc7_cache\encoder.txt` with the converter revision, `kTextureEncoderRev` in `src/texcache.h`), setup throws it away and
converts again; the cache file format is unchanged. `nx_texq` (a Metroid tool, `games/mpr/setup/texq.cpp`) measures the BC7 encoder alone; to measure the entries as written
(BC7, BC5 and RGBA8) use `test_texcache mkrefs` (`games/mpr/setup/tests`) and `scripts\check_texcache.py`.

Quality check on the game's own textures (developers): `games/mpr/setup/scripts/check_texquality.py --baseline` runs
`nx_texq` over all 4,413 ASTC textures and checks per-texture floors (PSNR RGB 33 dB, alpha 32 dB, opaque alpha exactly
255, SSIM 0.95; one white-noise texture is exempt). At quality 3 the minimum PSNR rose from 17.4 to 33.6 dB (19.2 dB for
the noise texture) and the mean from 48.4 to 50.0 dB; textures under 33 dB went from 65 to 1.

## If something goes wrong

- **"The keys cannot open this dump"**: your keys file is older than the game. Dump the keys again from a console on recent
  firmware.
- **"This is an update"**: the update was given without the base game. Give both.
- **"This version of the game is not supported"**: the message names the version found and the supported ones. Give the
  supported update with the base game, or leave an unsupported update out.
- **"different game"**: use the dump of this game.
- **"The dump is damaged"**: the file was not copied completely or is corrupt. Copy it again, or dump the game again. The tool
  checks the dump's own hashes before it writes anything.
- "Shader pack not found": put `shader_cache.mprshc` in the same folder as `mpr_setup.exe`, or start setup with
  `--shader-pack` pointing at the file. Without the shaders, the game draws nothing. If the install folder already has shaders
  from an earlier complete run, setup keeps them and does not need the pack.
- **"Not enough free space"**: free some space or pick another drive. Repeating setup is safe.
- **It stopped halfway** (power cut, crash): run it again. Finished files are kept.
- When you ask for help, send `setup.log` from the install folder. It contains no key data.

## Building it

The base builds the setup tool for one game: `pwsh base\tools\setup\scripts\build.ps1 -Game games\mpr -Test` (see the base's
`tools/setup/README.md` and `docs/BUILD.md`). Metroid's part of it, the Retro pak and TXTR readers, the program scan and the texture
list, is `hook.cmake` and the sources beside it.
