# nxrecomp

Tools for statically recompiling Nintendo Switch (AArch64) games into native Windows x64 programs.

- `recomp/`: the recompiler. It turns AArch64 code into C++ and checks the output against Unicorn instruction by instruction.
- `runtime/`: the runtime. It covers the memory map, module loader, threads, Horizon SVC and IPC handling, crash reports, and the NVN-to-Vulkan front end.
- `tools/`: offline tools for Maxwell shaders to SPIR-V, ASTC to BC7 textures, and Tegra block-linear tiling, plus the setup tool, the release builder and the test tools.

No game code or data is included. Games are supplied by the user from their own dumps.

The first game target is [Metroid Prime Remastered](https://github.com/drakolordx7/MetroidPrimeRemasteredRecomp), which includes this repository as a submodule. Game bugs and the player-facing README live there.

## Status

Early. The recompiler lifts all of Metroid Prime Remastered's code and passes its checks, and the runtime boots the game into its first level. Gaps are listed in the development logs under `runtime/docs/` and `recomp/docs/`.

## Building and testing

The prerequisites and the commands to run the tests are in [CONTRIBUTING.md](CONTRIBUTING.md). Each folder's own README goes into more detail.

## Contributing

Pull requests and bug reports are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) first. Never post game data, keys or dumps. Security problems go through [SECURITY.md](SECURITY.md).

## Licence

GPL-3.0. Borrowed code keeps its original licence notice; see each folder's README and third-party notices.
