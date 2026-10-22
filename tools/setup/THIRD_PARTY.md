# Third-party code and sources in tools/setup

This tool is GPL-3.0-or-later. Everything below is GPL-compatible.

## Code that is linked or shipped

| Component | Used for | Source | Licence |
|---|---|---|---|
| astcenc 5.7.0 (decoder DLLs `astcdec-*-shared.dll`) | ASTC decode inside the texture converter | github.com/ARM-software/astc-encoder, via `tools/textures` | Apache-2.0 (`../textures/licenses/LICENSE.astc-encoder.Apache-2.0`) |
| `bc7e.ispc` (compiled with ISPC) | BC7 encode | github.com/richgel999/bc7enc_rdo, via `tools/textures` | Apache-2.0 (`../textures/licenses/LICENSE.bc7enc_rdo`) |
| zstd 1.5.7 (`third_party/zstd`, fetched by `scripts/fetch_zstd.ps1`) | NSZ / XCZ and compressed shader packs | github.com/facebook/zstd | BSD-3-Clause, taken under that option (`licenses/LICENSE.zstd`, `licenses/NOTE.zstd.txt`) |
| Windows CNG (`bcrypt.dll`) | AES-ECB, SHA-1, SHA-256 | the operating system | none shipped |
| `tools/textures`, `tools/tiling` | transcoder and Tegra tiling libraries | this project | GPL-3.0-or-later |
| The runtime's cache hash (`tex_hash_seeded` in `src/texcache.cpp`) | cache keys that equal the game's | `runtime/src/gfx/nvn/nvn_texdata.cpp` | GPL-3.0-or-later, same project |

## Formats and behaviour read as documentation (no code copied)

The NCA, PFS0, HFS0, RomFS, NSO, ticket, XCI and NCZ readers in `src/` were written for this project from public format
descriptions, then checked against a real dump (the results are in `tools/docs/debug/020.md` and `021.md`).

| Source | What it told us | Licence of that source |
|---|---|---|
| Switchbrew wiki (NCA, PFS0, HFS0, RomFS, NSO, Ticket, XCI) | field layouts, key generation, the CTR counter | CC BY-SA 3.0 (text only, nothing copied) |
| hactool (SciresM) | key derivation order, header decryption (XTS with a big-endian sector number), section counter | ISC |
| LibHac (Thealexbarney) | integrity tree and RomFS table behaviour | MIT |
| nstool (jakcron) | cross-checking listings of the real dump | MIT |
| nsz (nicoboss) | NCZ layout: section table, block header, counter rule | MIT |
| refs/retrotool (Retro PACK, TXTR, LZSS) | pak directory, TXTR header and buffer table, LZSS | MIT |

If you ever copy code from one of these into this folder, add its notice here and keep the licence file in `licenses/`.
