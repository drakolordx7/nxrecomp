# NX

NX turns Nintendo Switch games into native Windows programs. It is for players who own a game and want a PC version with mouse and keyboard, any resolution and high refresh rates, and for developers who want a recompiler and runtime for Switch 1 games.

| Game | Status | Download |
| --- | --- | --- |
| [Metroid Prime Remastered](games/mpr) | Playable | [Latest release](https://github.com/drakolordx7/nxrecomp/releases/latest). Earlier versions, up to [v0.1.5](https://github.com/drakolordx7/MetroidPrimeRemasteredRecomp/releases/tag/v0.1.5), are in the old [MetroidPrimeRemasteredRecomp](https://github.com/drakolordx7/MetroidPrimeRemasteredRecomp) repository. |
| [Hyrule Warriors: Age of Calamity](games/hwaoc) | Planned | None yet |

No game files are included. You need your own dump of the game and the keys from your own Switch. Not affiliated with Nintendo, Retro Studios or Koei Tecmo.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit
- A GPU and driver with Vulkan 1.3. Only an NVIDIA GeForce RTX 3080 has been tested
- About 14 GB of free disk space for Metroid Prime Remastered
- Your dump of the game and your `prod.keys`. The version each game needs is in its own README

## Usage

1. Unzip the release into a folder with a short path and run the game's launcher (`mpr_launcher.exe` for Metroid Prime Remastered).
2. Press "Set up game files...", choose your dump and your `prod.keys`, and start the setup. It takes a few minutes.
3. When the launcher says "Ready to play.", press Play.

Controls, settings and fixes for common problems are in the game's README and in `HELP.txt` in the release.

## Reporting problems

[Open an issue](https://github.com/drakolordx7/nxrecomp/issues/new/choose) and attach the game's log (`mpr.log` for Metroid Prime Remastered; the next run keeps the old one as `.prev`). For a crash, attach the `_crash.txt` or `_fatal.txt` file too. Never attach game files, your dump or your keys.

## Contributing

- Open pull requests against `main`. Say what you changed and what you ran. Keep unrelated clean-ups out, and write commit subjects as one plain sentence. [CONTRIBUTING.md](CONTRIBUTING.md) has the code style and the checklist.
- `base/` holds the recompiler (`recomp/`), the runtime (`runtime/`) and the tools (`tools/`). It knows nothing about any one game. `games/<id>/` holds everything specific to one game. A game never reaches into another game's folder.
- Never put game data, keys or anything extracted from a game in a pull request.
- Build with Visual Studio Build Tools (C++ workload), LLVM 23 with clang-cl, CMake, Ninja, sccache, the Vulkan SDK, Python 3.13 with uv and the .NET 10 SDK. Run from `cmd` or PowerShell, not Git Bash. From the repository root:

```
git clone https://github.com/drakolordx7/nxrecomp
uv venv base\tools\.venv --python 3.13
uv pip install --python base\tools\.venv -r base\recomp\requirements.txt numpy pillow
pwsh base\runtime\scripts\build.ps1 -Test
pwsh -File base\tools\textures\scripts\fetch_third_party.ps1
pwsh -File base\tools\setup\scripts\fetch_zstd.ps1
pwsh -File base\tools\setup\scripts\build.ps1 -Game games\mpr
base\recomp\scripts\build_game.cmd games\mpr
pwsh base\runtime\scripts\build_game_exe.ps1 -Game games\mpr
```

The lift in `build_game.cmd` takes a long time the first time. Output goes to `build\`. [base/docs/BUILD.md](base/docs/BUILD.md) lists every script, flag and folder. Folders with an `oracle\LOCKED` file are checks that must not be edited.

## Credits

Maintained by [drakolordx7](https://github.com/drakolordx7).

Built on the work of others:

- Recompiler and runtime design: [smo-recomp](https://github.com/Coz-0/smo-recomp) (the lifter this one started from), [SuperMarioWonderRecomp](https://github.com/BelmanteGu) by Projeto Belmante, [ssbu-recomp](https://github.com/OpenSmash-Ultimate/ssbu-recomp) (C++ exceptions), [ScarletNX](https://github.com/scarletnx/ScarletNX) (service layouts) and [LASRecomp](https://github.com/peiikai/LASRecomp)
- [Ryujinx](https://ryujinx.app) (Ryubing): the Maxwell shader translator and the audio renderer port
- [Eden](https://eden-emu.dev) and yuzu: file system, audio and display service layouts, and Eden as the reference for checking frames
- [retrotool](https://github.com/PrimeDecomp/retrotool) and [PrimeRemasterStructs](https://github.com/PrimeDecomp/PrimeRemasterStructs) by PrimeDecomp: Metroid Prime Remastered file formats
- Libraries: [SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui), [volk](https://github.com/zeux/volk), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers), [astc-encoder](https://github.com/ARM-software/astc-encoder), [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), [zstd](https://github.com/facebook/zstd), AMD FidelityFX FSR 1, DejaVu Sans, and the [Kenney Input Prompts](https://kenney.nl/assets/input-prompts) icons

Contributors and testers: none yet. Everyone who sends a fix or a useful log goes here.

Licences of the third-party parts are in the `licenses` folder and `THIRD_PARTY_NOTICES.txt` of each release.

## License

GPL-3.0, see [LICENSE](LICENSE). Third-party components keep their own licenses.
