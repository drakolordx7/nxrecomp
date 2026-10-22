# S4e log: NVN correctness through to gameplay

S4e carries the NVN-on-Vulkan back end (S4d) from "the title screen renders and the device is lost in the loading screen" to the first frames of the game world. Developed on branch `nvn2`; the game under test is the real Metroid Prime Remastered run headless by `mpr.exe` (`.weaver/boot_nvn2.sh`).

## Result

The game now runs from the title screen through the main menu, New Game and the loading screen into the Frigate Orpheon: the opening cutscene over the planet, then the hangar with the HUD. A validation run of 420 s (about 22,000 presented frames, most of them in the world) logs 0 validation messages and no device loss. The first runs of this stream logged 268 messages of 15 kinds and lost the device about a minute into the loading screen. Debug note 140 lists every message and its fix, `docs/nvn_struct_sizes.md` has the struct audit, and "Where the run gets to" below says what is still wrong on screen.

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
| Occlusion counters | `nvn_replay.cpp` (`ResetCounter`, `ReportCounter`) | a Vulkan occlusion query from ResetCounter to ReportCounter (precise when the device has it); the 64-bit sample count is copied into the report. Statistics counters other than samples passed, primitives generated and feedback primitives written are reported as non-zero (the last two are counted, see Open points) |
| Sparse (virtual) textures | `api_memory.cpp`, `api_texture.cpp`, `nvn_texture_layout.*` | `nvnMemoryPoolMapVirtual` succeeds (sparse textures are ordinary resident images that the game fills with copy commands); `GetSparseTileLayout` returns the standard 64 KiB tile shapes |
| Transform feedback | `api_cmd_work.cpp`, `nvn_replay.cpp`, `nvn_pipeline.cpp`, `nvn_spirv_patch.*` | Begin / End and the feedback buffer are executed with VK_EXT_transform_feedback. The vertex shader gets a variant (`spirv_add_xfb`) that captures its Location outputs in Location order into buffer 0, each narrowed to the components the shader stores (a vec4 with xyz written is 12 bytes), and the pipeline has no fragment stage. The console's real layout lives in the driver's compile data and is not in the shader binary; this layout matches the one stream-out shader checked (shadow decals, 12 bytes per vertex). A pipeline change inside a section ends feedback and the next begin continues after the bytes already written. Draws in a section that have no variant and no attachment are skipped. The stream-out counters (13 and 14) are counted on the CPU (see Open points) |
| Storage usage | `nvn_gpu_texture.cpp` | every uncompressed colour image keeps the storage usage when the GPU accepts it. The game does not set the builder's IMAGE flag on the fog volume that a vertex shader writes; without the usage the write went to a dummy image, the volume stayed empty, its transmittance read as zero and the whole world rendered black |
| Depth-only passes | `nvn_spirv_patch.*`, `nvn_pipeline.cpp` | a pass without colour attachments runs fragment shaders whose outputs became private variables (`spirv_outputs_to_private`), so nothing writes to a missing attachment |
| NVN 55 names | `scripts/gen_nvn_table.py`, `api_*.cpp` | the 11 names the game asks for by name are table entries with no-op implementations |

## Tools

| Tool | Use |
|---|---|
| `.weaver/boot_nvn2.sh`, `.weaver/trun.sh`, `.weaver/vrun.sh` | boot.py against this worktree in `work/run_nvn2`; `trun.sh` runs without validation, `vrun.sh` with it and prints a summary (`.weaver/vsum.py`) |
| `A#<n>` in `NXRT_BOOT_PRESS` | a scripted press at presented frame n instead of second n (a loaded machine plays the same sequence). Added to `game-build/src/mpr_main.cpp` |
| `NXRT_NVN_TRACE_FRAME=<n>` (`[nvn] trace_frame`) | logs every draw, clear, copy and dispatch of the frame presented as number n (`nvn.trace`): programs, render targets, sampled textures, state; afterwards dumps every render target it drew to as a PNG with per-channel statistics (`trace_t<id>_<format>.png`, HDR values shown as x / (1 + x)) |
| `NXRT_NVN_DUMP_MISSING=<dir>` (`[nvn] dump_missing`) | writes every shader block the cache lacks in the corpus layout, ready for `spirv_harness --batch` |
| `NXRT_NVN_TRACE_SNAP=<draws>` (`[nvn] trace_snap`) | with a traced frame: after each listed draw number the pass is closed and the colour and depth targets are copied; written after the frame as `snap_d<draw>_t<id>_<format>.png` with statistics. Mid-frame views without a graphics debugger |
| `NXRT_NVN_TRACE_SNAP_FS=<hash prefixes>`, `NXRT_NVN_TRACE_SNAP_SAMPLED=1` | the same snapshot after every draw whose fragment shader hash starts with a prefix; with `SAMPLED` the uncompressed textures that draw samples are copied too (a 3D texture as all its slices stacked into one tall image) |
| `NXRT_NVN_TRACE_AFTER_FS=<hash prefix>`, `NXRT_NVN_TRACE_MIN_SEC=<s>` | traces the frame after the first one that draws with that fragment shader (after that many seconds of run time). A frame number differs from run to run, a shader does not |
| trace lines | every traced draw lists the first words of the vertex and fragment uniform blocks, the vertex count, and for every sampled texture the share of its storage in game memory that is non-zero (`mem 0%`: the game never wrote it, so it is a render target or a movie plane) |

## Tests

New or changed groups: `gfx_nvn_prim_counters` (primitives per draw mode, counters 13 and 14 through the replay), `gfx_nvn_scene_state` (partial stencil clear with and without depth, occlusion counts), `gfx_nvn_texgpu` (3D transitions without warnings, raw-bit aliases, sampled layout, dummy storage views), `gfx_nvn_spirv_patch` (the four patches on real shaders, spirv-val: extra outputs, dual-source, stream-out variant, private outputs), `gfx_nvn_texture` (sparse tile shapes), `gfx_nvn_cmd_work`, `gfx_nvn_memory`, `gfx_nvn` (struct sizes).

## Where the run gets to

| Stage | State | Screenshot (`work/run_nvn2/final/`) |
|---|---|---|
| Title | "PRESS A TO START" correct; the background is black (a movie, see open points) | `title.png` |
| Menu / file screen | correct | `menu_hud_screen.png` |
| Opening cutscene | nebula, planet, asteroids and stars render and are lit | `opening_cutscene_nebula.png`, `opening_cutscene_planet.png`, `gameplay_planet_cutscene.png` |
| Hangar with HUD | geometry, lighting, fog, bloom, tone mapping and HUD render; the door panel shows colour noise (open points) | `gameplay_hangar_hud.png`, `gameplay_hangar_hud_2.png`, `validation_run_world.png` |

Frame structure of a world frame, found with the trace tools: depth prepass, a 646x32 R32_UINT pass with the light lists, the forward pass into an R11G11B10F target, a fog pass (an 800x456 target, then a vertex shader that writes one froxel column per vertex into a 100x57x64 RGBA16F volume, then a full-screen composite), a bloom mip chain, tone mapping with two 33x33x33 grading volumes into the swap chain texture, the HUD.

## Open points

- Title background: the first draw of the title frame is a full-screen quad that samples an R8 1600x900 texture and an RG8 800x450 texture, both with no data in game memory. Their sizes are those of the two planes of a 4:2:0 video frame, so the background is a movie (the media stream's work), not a missing NVN path.
- A rectangle of saturated colour noise covers the door panel of the hangar. It is written into the HDR target by draws of the fragment shader `8b08a5cc12e27480` (vertex shader `87c66db517720684`), in the same pass and with the same textures as neighbouring draws that are correct, so the bad value comes from data or uniforms of those draws, not from a texture. Checked and ruled out: the fog volume (now written), tone mapping and the grading volumes (correct), the sampled textures (identical in clean and bad draws). Not yet checked: the fragment uniform blocks of the bad draws against a reference. The shader loops over the lights of a tile read from the R32_UINT light grid and samples a BC6H texture array of 2888x2888x4.
- Stream-out counters 13 and 14 are counted on the CPU in the replay: every draw adds its primitives (per mode and instance, `primitives_for` in `nvn_enums.cpp`) to 13, and a draw inside a transform feedback section also adds them to 14; `ResetCounter` zeroes the counter of its type. Indirect draws are not counted (noted once in the unsupported report).
- RenderDoc cannot capture the game: the capture layer does not offer VK_EXT_external_memory_host, which the back end needs for the imported pools. The trace and snapshot tools above replace it.
- The shader cache survey missed some blocks (two were translated in this stream). `[nvn] dump_missing` writes any block the cache lacks in the layout `spirv_harness --batch` reads.
- The lift tests (`tests/lift`) are off in this worktree (no `../recomp` next to it), so they were not run here.
