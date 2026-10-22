# Metroid Prime Remastered PC: first-run setup

The game ships without any Nintendo data. This tool builds the game folder from your own copy of the game.
You run it once. It needs about 13 GB of free disk space and takes about 5 minutes on a modern PC.

## What you need

- A dump of the game, version 1.0.0, made from your own Switch: an `.nsp` (eShop) or `.xci` (cartridge) file.
  `.nsz` and `.xcz` work too. Updates and patches are not supported. The base game is title id `010012101468C000`.
- Your Switch keys (`prod.keys`), dumped from your own console with Lockpick_RCM. The tool only reads the file and never
  copies or uploads it. If you use an emulator (yuzu, Eden, Ryujinx and similar), the tool finds its keys by itself.
  If your eShop dump was bought on another console, put that console's title key in a `title.keys` file next to `prod.keys`.
- 13 GB of free space on the drive you install to.

## Run it

1. Start `mpr_setup.exe`.
2. Pick your dump file, or drag it into the window. Check the green line under it.
3. Pick the keys file if it was not found, and the install folder.
4. Press **Start setup**. You can press **Stop** at any time and run it again later; it carries on where it left off.

When it finishes, the install folder has everything the game needs. Start the game from that folder.

### Command line

```text
mpr_setup_cli --input "Metroid Prime Remastered.nsp" --out D:\MPR [--keys prod.keys] [--quality 3] [--threads 8]
mpr_setup_cli --input dump.nsp --check            # only look at the dump, write nothing
mpr_setup_cli --find-keys                         # list key files it can find
```

Exit codes: 0 done, 1 error, 2 the dump cannot be used, 3 keys problem, 4 disk problem, 5 stopped (run it again to continue).

## What it makes

```
<install folder>\
  exefs\            the game program (copied from the dump unchanged)
  romfs\            the game's data files
  bc7_cache\        the game's textures converted to a format PC graphics cards read directly (about 4.4 GB)
  shader_cache\     translated shaders (only when this build comes with them)
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
| 3 (default) | about 3 minutes | the value the game is tuned for |
| 5 | about 11 minutes | slightly better on noisy textures |

## If something goes wrong

- **"The keys cannot open this dump"**: your keys file is older than the game. Dump the keys again from a console on recent
  firmware.
- **"This is an update"** or **"different game"**: use the original v1.0.0 base game dump.
- **"The dump is damaged"**: the file was not copied completely or is corrupt. Copy it again, or dump the game again. The tool
  checks the dump's own hashes before it writes anything.
- **"Not enough free space"**: free some space or pick another drive. Repeating setup is safe.
- **It stopped halfway** (power cut, crash): run it again. Finished files are kept.
- When you ask for help, send `setup.log` from the install folder. It contains no key data.

## Building it

```powershell
pwsh -File tools\textures\scripts\fetch_third_party.ps1   # once: astcenc, bc7enc_rdo, ISPC
pwsh -File tools\setup\scripts\fetch_zstd.ps1             # once: zstd (for NSZ/XCZ)
pwsh -File tools\setup\scripts\build.ps1 -Test            # CMake + Ninja + clang-cl, then the unit tests
```

You need Visual Studio Build Tools, LLVM (clang-cl), CMake and Ninja. The program is plain Win32 so it does not depend on
a graphics driver and works on a machine whose driver is not set up yet. Licences of the parts it uses are in `THIRD_PARTY.md`.
