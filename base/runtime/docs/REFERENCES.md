# References outside this repository

Comments and docs here cite a few documents that do not live in the base repository. This list says where each one is. Paths under
`games/` and `refs/` are relative to the NX root; `refs/` holds read-only clones of other projects.

## Metroid Prime Remastered work logs (games/mpr)

The graphics work was done on Metroid Prime Remastered, and its session and debug logs moved to the game's repository. The fixes they
describe are generic and stay in base; the `(Snn)` and `(debug nnn)` tags in code point at these logs.

| Cited as | Where it is | What it holds |
|---|---|---|
| `S10` | `games/mpr/docs/gfx/S10.md` | Display options and frame-time performance on the title screen. |
| `S24` | `games/mpr/docs/gfx/S24.md` | Pipeline-creation hitches on a first run: prewarm, pipeline libraries, shader preparation. |
| `S29` | `games/mpr/docs/gfx/S29.md` | What the pipeline libraries cost, and the whole-pipeline upgrade that replaced them. |
| `S38` | `games/mpr/docs/gfx/S38.md` | GPU time in the open scenes: device-local pool copies (`[nvn] pool_mirror`) and tracked pass barriers (`[nvn] pass_barriers`). |
| `debug 121` | `games/mpr/docs/gfx/debug/121.md` | NVNtextureView is 16 bytes wide in this game (see `docs/nvn_struct_sizes.md`). |
| `debug 123` | `games/mpr/docs/gfx/debug/123.md` | Device loss after the title screen (closed). |
| `debug 140` | `games/mpr/docs/gfx/debug/140.md` | The first gameplay path through the NVN back end. |
| `debug 440` to `443` | `games/mpr/docs/gfx/debug/440.md` ... `443.md` | Texture objects over one storage, a draw sampling its own attachment, the integer-attribute patch, the environment-map mip chain. |
| `docs/mpr/survey/SURVEY.md` | `games/mpr/docs/mpr/survey/SURVEY.md` | The game's survey, including the texture layouts it uses. |

## SuperMarioWonderRecomp (refs/SuperMarioWonderRecomp)

| Cited as | Where it is | What it holds |
|---|---|---|
| `docs/nvn/DEBUG.md` | `refs/SuperMarioWonderRecomp/docs/nvn/DEBUG.md` | NVN command-buffer and handle behaviour found while debugging that project. |
| `docs/nvn/TILING.md` | `refs/SuperMarioWonderRecomp/docs/nvn/TILING.md` | Tegra block-linear tiling. |
| `docs/nvn/TRANSLATOR.md` | `refs/SuperMarioWonderRecomp/docs/nvn/TRANSLATOR.md` | The shader translator's bank and binding layout. |
