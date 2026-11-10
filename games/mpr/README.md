# Metroid Prime Remastered for Windows (unofficial PC port)

A Windows port of Metroid Prime Remastered, made by recompiling the Nintendo Switch release (version 1.0.0) into a native x64 program. It comes with a launcher and a setup tool, and supports mouse and keyboard, any resolution or aspect ratio, and high refresh rates.

[Download v0.1.5](https://github.com/drakolordx7/MetroidPrimeRemasteredRecomp/releases/tag/v0.1.5). Releases up to v0.1.5 are in the old repository; later ones will be on this repository's releases page.

The release has no game files. You need your own dump of Metroid Prime Remastered version 1.0.0 and the keys from your own Switch. Not affiliated with Nintendo or Retro Studios.

This folder has the game's manifest (`game.json`), hook sites (`recomp/`), runtime module (`runtime/`), setup hook (`setup/`), release data (`release/`) and docs. It builds on the shared [`base/`](../../base). Build steps and credits are in the [main README](../../README.md).

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit
- A GPU and driver with Vulkan 1.3. It has been tested on an NVIDIA GeForce RTX 3080 only; AMD and Intel GPUs are untested
- About 14 GB of free disk space
- Your dump of the game, version 1.0.0 (`.nsp`, `.xci`, `.nsz` or `.xcz`; updates and patches are not supported) and your `prod.keys` (if the dump was bought on another console, also its `title.keys`, next to `prod.keys`)

## Usage

1. Unzip the release into a folder with a short path (under about 180 characters) and run `mpr_launcher.exe`.
2. Press "Set up game files...", choose your dump and your `prod.keys`, and press Start setup. It takes about 5 minutes on a 16-core PC, longer on fewer cores. You can stop it and run it again later.
3. When setup closes, the launcher prepares shaders for your GPU (a few minutes, once per driver). When it says "Ready to play.", press Play.

Settings are in the launcher (display, mouse, keyboard, controller, button icons) and on the F1 page in the game, and are saved to `controls.ini` in the game folder. Saves are in `%APPDATA%\MPRRecomp\save`. Logs (`mpr.log`, `setup.log`) are in the game folder. To update, unzip a new release over the old folder. To uninstall, delete the folder and the save folder.

Defaults: WASD move, mouse aims, left button fires, right button or Q locks on, middle button or F fires missiles, Space jumps, Ctrl or C is the morph ball, Tab or M opens the map, Escape pauses, 1 to 4 pick the beam and 5 to 8 the visor. Xbox, PlayStation and Switch controllers work. Alt+Enter toggles fullscreen and F2 shows the frame rate. `HELP.txt` in the release lists every key, every setting and the fixes for common problems. Known problems are in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Reporting problems

[Open an issue](https://github.com/drakolordx7/nxrecomp/issues/new/choose) and attach `mpr.log` from the run where it happened (the next run keeps the old one as `mpr.log.prev`). The top right of the launcher and the "mpr 0.1.0" line near the top of the log show your version. For a crash, also attach `mpr_crash.txt` or `mpr_fatal.txt`. Never attach game files, your dump or your keys.

## License

GPL-3.0, see [LICENSE](../../LICENSE). Credits are in the [main README](../../README.md).
