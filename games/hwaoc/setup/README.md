# Age of Calamity setup

`hwaoc_setup` and `hwaoc_setup_cli` are the base setup tool (`base/tools/setup/README.md`) built for this game from `game.json`. The game
needs no setup hook: the base installer does everything, so there is no `setup_hook` in the manifest and no game code in this folder.

- Accepted: game version 1.4.1 only (main build ID `E5F275B5D0889899E55DE1A164EEF060D547B0A8`), made of the base game (title id
  `01002B00111A2000`) and the 1.4.1 update (`01002B00111A2800`, BKTR patch RomFS). The base game alone, or any other update, is refused with the
  version found and the one wanted. NSP, XCI, NSZ and XCZ; DLC (`01002B00111A3001` to `3004`) is optional.
- Result: `exefs/` (the update's NSOs, byte-identical to the dump), `romfs/` (1,102 files, the update applied; same files as `game/extract/update/romfs`),
  `aoc/<title id>/` for each DLC, `shader_cache/spirv/` from `shader_cache.hwaocshc`, `hwaoc.ini`, `setup.log`.
- No textures stage and no `programs.bin`: the runtime reads the game's textures as they are, and the game's shaders are Maxwell binaries the game
  hands to NVN at run time, so there is no list to compile ahead. The launcher therefore has no "Prepare shaders" step for this game.
- Needs about 14 GB free. Measured on a dev PC with an SSD: 138 s for the whole install.
- The shader pack is made from a shader cache with `base/tools/setup/scripts/make_shader_pack.py <cache> <pack> --zstd`; the cache comes from
  `games/hwaoc/shaders.sh` after boot runs that dumped the missing blocks.
