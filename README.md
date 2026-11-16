# NX

Nintendo Switch games ported to Windows and Linux by recompiling their code into native x86-64 programs.

| Game | Status | Download |
| --- | --- | --- |
| [Metroid Prime Remastered](games/mpr) | Working on Windows and Linux | [v0.1.5](https://github.com/drakolordx7/MetroidPrimeRemasteredRecomp/releases/tag/v0.1.5) (Windows only; Linux comes with the next release) |
| a game | Working | Not released yet |
| [Hyrule Warriors: Age of Calamity](games/hwaoc) | In progress: boots, plays its movies and its first battle | Not released yet |

No game files are included. You need your own dump of the game and the keys from your own Switch. Not affiliated with Nintendo or the games' developers and publishers.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit, or a 64-bit Linux with glibc 2.35 or newer (Ubuntu 22.04, SteamOS 3, Fedora, Arch and similar)
- A GPU with Vulkan 1.3 (on Linux: a current Mesa or NVIDIA driver)
- Your own dump of the game and your `prod.keys`

Each game's page lists the game version it needs and how much disk space it takes.

## Usage

1. Unpack the release into a folder (the `.zip` on Windows, the `.tar.xz` on Linux) and run its launcher.
2. Press "Set up game files...", pick your dump and your `prod.keys`, and start the setup.
3. When the launcher says "Ready to play.", press Play.

Controls, settings and known problems are on each game's page and in `HELP.txt` in the release.

## Reporting problems

[Open an issue](https://github.com/drakolordx7/nxrecomp/issues/new/choose) with the game, its version and the log from the run where it happened. The log is next to the exe, and the one from the run before is kept as `.prev`. For a crash, attach the `_crash.txt` or `_fatal.txt` file as well. Never attach game files, your dump or your keys.

## Contributing

- `base/` has the recompiler (`recomp/`), the runtime (`runtime/`) and the tools (`tools/`) that every game uses. Everything specific to one game is in `games/<id>/`.
- Open pull requests against `main` and say what you changed and how you tested it. [CONTRIBUTING.md](CONTRIBUTING.md) has the code style and the checklist.
- Never put game data, keys or anything extracted from a game in a pull request.
- Windows and Linux build from the same code. Anything that talks to the operating system goes through `base/runtime/src/os/` (one file per system), so a change works on both.
- To build on Windows, you need Visual Studio Build Tools (C++), LLVM with clang-cl, CMake, Ninja, sccache, the Vulkan SDK, Python 3.13 with uv and the .NET 10 SDK. These commands build Metroid Prime Remastered from `cmd` or PowerShell in the repository root:

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

On Linux you need clang and lld (version 23), CMake, Ninja, Python 3, ispc, the Vulkan, SDL and FFmpeg development packages (`base/docs/BUILD.md` lists them). This builds the runtime with its tests, then Metroid Prime Remastered:

```
python3 base/scripts/nx_build.py main --test .
python3 base/scripts/nx_build.py main --game mpr --libs --setup
```

The first `build_game.cmd` (or `--libs`) run is slow. [base/docs/BUILD.md](base/docs/BUILD.md) covers every script and option.

## Credits

Made by [drakolordx7](https://github.com/drakolordx7), with code and research from:

- [smo-recomp](https://github.com/Coz-0/smo-recomp), the recompiler this one grew out of, plus [SuperMarioWonderRecomp](https://github.com/BelmanteGu) by Projeto Belmante, [ssbu-recomp](https://github.com/OpenSmash-Ultimate/ssbu-recomp) (C++ exceptions), [ScarletNX](https://github.com/scarletnx/ScarletNX) (service layouts) and [LASRecomp](https://github.com/peiikai/LASRecomp)
- [Ryujinx](https://ryujinx.app) (Ryubing) for the Maxwell shader translator and the audio renderer
- [Eden](https://eden-emu.dev) and yuzu for the file system, audio and display service layouts; we also check our frames against Eden
- [retrotool](https://github.com/PrimeDecomp/retrotool) and [PrimeRemasterStructs](https://github.com/PrimeDecomp/PrimeRemasterStructs) by PrimeDecomp for the Metroid Prime Remastered file formats
- [SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui), [volk](https://github.com/zeux/volk), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers), [FFmpeg](https://ffmpeg.org), [astc-encoder](https://github.com/ARM-software/astc-encoder), [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), [zstd](https://github.com/facebook/zstd), AMD FidelityFX FSR 1, DejaVu Sans and the [Kenney Input Prompts](https://kenney.nl/assets/input-prompts)

Send a fix or a useful log and your name goes on this list. Each release includes the third-party licences (the `licenses` folder and `THIRD_PARTY_NOTICES.txt`).

## License

GPL-3.0, see [LICENSE](LICENSE). Third-party code keeps its own licence.
