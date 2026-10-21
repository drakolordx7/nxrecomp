# nxrecomp

Tools for statically recompiling Nintendo Switch (AArch64) games into native Windows x64 programs.

- `recomp/`: the recompiler. It turns AArch64 code into C++ and checks the output against Unicorn instruction by instruction.
- `runtime/`: the runtime. It covers the memory map, module loader, threads, Horizon SVC and IPC handling, crash reports, and the NVN-to-Vulkan front end.
- `tools/`: offline tools for Maxwell shaders to SPIR-V, ASTC to BC7 textures, and Tegra block-linear tiling.

No game code or data is included. Games are supplied by the user from their own dumps.

The project is GPL-3.0. Borrowed code keeps its original licence notice; see each folder's README and third-party notices.

The first game target is Metroid Prime Remastered.
