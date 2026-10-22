# S4e log: NVN correctness through to gameplay

S4e carries the NVN-on-Vulkan back end (S4d) from "the title screen renders and the device is lost in the loading screen" to the first frames of the game world. Developed on branch `nvn2`; the game under test is the real Metroid Prime Remastered run headless by `mpr.exe` (`boot_nvn2.sh`).

## Result

See the table at the end of "Where the run gets to". Debug note 140 lists every validation message and its fix; `docs/nvn_struct_sizes.md` has the struct audit.

## Struct sizes

The practice mod's `nvn.h` is for an older NVN than the game's (API 55). The game's own classes are the authority: an object it embeds next to its own fields cannot be larger than the room it leaves. The audit read every NVN object the game embeds in a longer-lived object (IDA, `main.elf`) and found these disagreements with the header:

| struct | header | game |
|---|---|---|
| NVNtexture | 0xC0 | 0x80 |
| NVNprogram | 0xC0 | 0x80 |
| NVNsampler | 0x30 | 0x40 |
| NVNtextureView | 0x28 | 0x10 (debug note 121) |
| NVNsamplerBuilder | 0x60 | at most 0x40 |

Agreeing with the header: device, queue, window, memory pool, buffer, sync, command buffer, texture and sampler pool and the colour, depth-stencil, polygon, channel-mask and multisample state objects. The runtime header and `nvn_sizes.inc` carry the game's values (`GAME_SIZES` in `scripts/gen_nvn_table.py`, source mask 8), and `gfx_nvn.object_sizes` asserts them. The back end's private layouts all fit the smaller sizes (static asserts). Builders other than the sampler builder cannot be pinned from the game; the evidence and the limits are in `docs/nvn_struct_sizes.md`.

`NVNtextureSparseTileLayout` is 16 bytes (four words); the header had a one-byte placeholder, which the game's 16-byte buffer made harmless but the back end's old `memset` wrote one byte only.

## What was added

| Area | Where | Behaviour |
|---|---|---|
| Storage images | `nvn_replay.cpp` (`resolve_image`, `build_set`), `nvn_gpu_texture.cpp` (`dummy_storage_view`) | `nvnCommandBufferBindImages` handles become storage-image descriptors (GENERAL layout, shader's image format); a 1x1 dummy when nothing usable is bound. Compute shaders that read a uniform block through their c[0] descriptor (`UniformAsStorage`) get that block's buffer as a storage buffer |
| Depth sampled as attachment | `nvn_replay.cpp` (`ensure_pass(depth_ro)`) | the pass keeps its depth attachment DEPTH_STENCIL_READ_ONLY_OPTIMAL while a draw samples it; depth textures are always sampled in that layout |
| Integer 32-bit colour formats | `nvn_gpu_texture.cpp` (`float_alias`, `attachment_format_of`) | rendered and sampled through float / uint / sint aliases of the same size, as the console moves raw bits |
| Vertex input | `nvn_replay.cpp` (`build_key`), `nvn_gpu_pool.cpp` | every shader input location exists; unset attributes read (0,0,0,1) |
| SPIR-V patches | `nvn_spirv_patch.*`, `nvn_pipeline.cpp` | extra producer outputs for varyings the consumer reads; the dual-source second output at Location 0, Index 1 |
| Masked depth/stencil clear | `nvn_mask_clear.*` | partial stencil write mask by a draw |
| Occlusion counters | `nvn_replay.cpp` (`ResetCounter`, `ReportCounter`) | a Vulkan occlusion query from ResetCounter to ReportCounter (precise when the device has it); the 64-bit sample count is copied into the report. Statistics counters other than samples passed are reported as non-zero |
| Sparse (virtual) textures | `api_memory.cpp`, `api_texture.cpp`, `nvn_texture_layout.*` | `nvnMemoryPoolMapVirtual` succeeds (sparse textures are ordinary resident images that the game fills with copy commands); `GetSparseTileLayout` returns the standard 64 KiB tile shapes |
| Transform feedback | `api_cmd_work.cpp`, `nvn_replay.cpp` | Begin / End / Pause / Resume and the feedback buffers are recorded and logged (`nvn.xfb`); the stream-out itself is not executed (see "Open points") |
| NVN 55 names | `scripts/gen_nvn_table.py`, `api_*.cpp` | the 11 names the game asks for by name are table entries with no-op implementations |

## Tools

| Tool | Use |
|---|---|
| `boot_nvn2.sh`, `trun.sh`, `vrun.sh` | boot.py against this worktree in `work/run_nvn2`; `trun.sh` runs without validation, `vrun.sh` with it and prints a summary (`vsum.py`) |
| `A#<n>` in `NXRT_BOOT_PRESS` | a scripted press at presented frame n instead of second n (a loaded machine plays the same sequence). Added to `game-build/src/mpr_main.cpp` |
| `NXRT_NVN_TRACE_FRAME=<n>` (`[nvn] trace_frame`) | logs every draw, clear, copy and dispatch of the frame presented as number n (`nvn.trace`): programs, render targets, sampled textures, state; afterwards dumps every render target it drew to as a PNG with per-channel statistics (`trace_t<id>_<format>.png`, HDR values shown as x / (1 + x)) |
| `NXRT_NVN_DUMP_MISSING=<dir>` (`[nvn] dump_missing`) | writes every shader block the cache lacks in the corpus layout, ready for `spirv_harness --batch` |
| `NXRT_NVN_TRACE_SNAP=<draws>` (`[nvn] trace_snap`) | with a traced frame: after each listed draw number the pass is closed and the colour and depth targets are copied; written after the frame as `snap_d<draw>_t<id>_<format>.png` with statistics. Mid-frame views without a graphics debugger |

## Tests

New or changed groups: `gfx_nvn_scene_state` (partial stencil clear with and without depth, occlusion counts), `gfx_nvn_texgpu` (3D transitions without warnings, raw-bit aliases, sampled layout, dummy storage views), `gfx_nvn_spirv_patch` (both patches on real shaders, spirv-val), `gfx_nvn_texture` (sparse tile shapes), `gfx_nvn_cmd_work`, `gfx_nvn_memory`, `gfx_nvn` (struct sizes).
