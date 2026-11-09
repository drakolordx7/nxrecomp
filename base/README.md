# nxrecomp

Tools for statically recompiling Nintendo Switch (AArch64) games into native Windows x64 programs.

- `recomp/`: the recompiler. It turns AArch64 code into C++ and checks the output against Unicorn instruction by instruction.
- `runtime/`: the runtime. It covers the memory map, module loader, threads, Horizon SVC and IPC handling, crash reports, and the NVN-to-Vulkan front end.
- `tools/`: offline tools for Maxwell shaders to SPIR-V, ASTC to BC7 textures, and Tegra block-linear tiling, plus the setup tool, the release builder and the test tools.

No game code or data is included. Games are supplied by the user from their own dumps.

A game is a folder, `games/<id>`, that the base takes as input: its manifest `game.json` (names, ids, builds, modules, file names), its lift config and hook sites in `recomp/`, its runtime module in `runtime/`, and its own docs and tools. The base builds, sets up and releases any such folder (`docs/BUILD.md`). Game bugs and the player-facing README live in the game's folder.

## Status

The recompiler lifts the whole code of the games built on it and passes its checks; the runtime boots them. Gaps are listed in the development logs under `runtime/docs/` and `recomp/docs/`.

## Building and testing

The prerequisites and the commands to run the tests are in [CONTRIBUTING.md](CONTRIBUTING.md). Each folder's own README goes into more detail.

## Contributing

Pull requests and bug reports are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) first. Never post game data, keys or dumps. Security problems go through [SECURITY.md](SECURITY.md).

## Licence

GPL-3.0. Borrowed code keeps its original licence notice; see each folder's README and third-party notices.
