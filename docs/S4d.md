# S4d notes: NVN on Vulkan

Part S4d of the Metroid Prime Remastered recompilation. Scope: real implementations of the NVN API on Vulkan 1.3 behind the logging shell of S4b (`nvnBootstrapLoader` hands out the functions), the offline translation of every Maxwell shader block of the game, and the oracles that prove both. Branch `nvn`, merged by the maintainer.

## Result (2026-10-21)

- 513 of the 523 NVN entry points are implemented, 10 are logging stubs. The game's own binary calls 204 NVN functions (read from its pointer table with IDA); all 204 are implemented. The table is at the end of this file.
- A "game" written against the practice mod's `nvn.h`, calling NVN only through `nvnBootstrapLoader` and `nvnDeviceGetProcAddress`, renders 8 oracle families with real translated shaders and real textures of the game (clear, triangle, geometry and rasterization, blend/depth/stencil/targets, textures and copies, commands/sync/frames, multisample). All pass with zero Vulkan validation errors. Rerun commands are in "Oracles".
- Offline shader translation: all 67,506 distinct blocks translate and validate (`spirv-val --target-env vulkan1.3`), 0 failures. The runtime key is the hash of the code block alone: 13,016 distinct code blocks (see "Shader cache").
- `PIXLD.MY_INDEX` is the sample index. Ryujinx left it unimplemented; the harness now lowers it to `gl_SampleID`.

## What exists

| Part | Files (`src/gfx/nvn/` unless noted) | Notes |
|---|---|---|
| Typed thunks | `nvn_abi.h/.cpp` | An NVN function is a C++ function with the types of `nvn.h`; `NVN_REG(fn)` builds the AArch64 thunk and checks the argument classes against the generated table. `abi::Caller` calls a thunk the way a game does (tests). |
| Plain-data objects | `api_builders`, `api_sampler`, `api_texture_builder`, `api_blend_color`, `api_depth_poly_ms`, `api_vertex`, `nvn_decoded.h` | Builders, state objects, samplers and views live entirely in the game's object memory (a private layout each, always inside the NVN struct size). The back end reads them through `decode()`. |
| Host objects | `api_device`, `api_memory`, `api_texture`, `api_program`, `api_cmdbuf`, `nvn_core.h`, `nvn_objects.cpp` | Heavy objects keep a reference-counted host object behind a 16-byte header in the first bytes of the game's object. |
| Recording | `api_cmd_state`, `api_cmd_work`, `nvn_cmd.h`, `nvn_cmd_handles.cpp` | 146 command buffer functions append records to a `CmdList`; a handle stays valid until the game records over the command memory it used (estimated), its pool is finalized, or its command buffer is. No count limit. |
| GPU core | `nvn_gpu.cpp`, `nvn_gpu_internal.h`, `include/nxrt/nvn_backend.h` | Render thread, four submission slots (command buffer, descriptor pools, upload memory), one timeline semaphore, deferred destruction. |
| Memory pools | `nvn_gpu_pool.cpp` | A pool is imported as Vulkan memory with `VK_EXT_external_memory_host`. A GPU address is a host address; buffers are zero-copy and anything the GPU writes (copies, counters, events, compute) is visible to the CPU through the same pages. Measured on the RTX 3080 and the UHD 770 with `MapViewOfFile` views of one section: both import, and GPU writes show through every view. |
| Textures | `nvn_texture_layout`, `nvn_texdata`, `nvn_gpu_texture.cpp` | Layout and sizes with `tools/tiling` (equal to the real GPU buffer size for all 6,984 survey rows). Block-linear data is untiled on first use; BCn passes through; ASTC is transcoded to BC7 once with `tools/textures` and cached on disk by a hash of the source bytes. Images, views, samplers, layout tracking. |
| Shaders | `nvn_shaders.h/.cpp`, `tools/shaders` | Translated SPIR-V by code hash, reflection of its bindings, the translator's support buffer. |
| Replay | `nvn_replay.cpp`, `nvn_pipeline.h/.cpp`, `nvn_mask_clear.*` | The interpreter of recorded lists: persistent GPU state, lazily opened dynamic-rendering passes, pipelines keyed by baked state (the rest is dynamic state of Vulkan 1.3), descriptor sets from the shaders' binding tables, masked clears by a draw. |
| Present | `nvn_present.cpp`, `nvn_present_pass.*` | The window texture is drawn into the Renderer's frame (crop honoured) after the shell's `FrameLimiter::begin_frame()`. |
| Device layer | `vk_device.cpp`, `gfx.h` | Device created with every Vulkan 1.0 to 1.3 feature the GPU has (minus robustness) plus the extension features the translated shaders and the back end need. |

## Design decisions

- Pool memory is imported, not copied. smo and Wonder copy buffer ranges into an upload ring at replay time and copy compute results back. Importing the game's pages removes the ring, the write-back tracking and the stale-copy bugs those projects document (Wonder DEBUG #11, #30, #32, #53, #56). Uniform and vertex data are read by the GPU from system memory over PCIe; no frame-time cost showed up in the 120-frame scene (0.6 ms of CPU per frame), but no real-game frame has been measured.
- The back end is Wonder's design in new code, with smo's recording model. The plan named smo's backend as the base. smo's front end (`nvn_gl.c`, 400 KB) is entangled with an OpenGL path and write-watch texture tracking; Wonder's is Vulkan only, MIT, C++ and close to this game's SDK. Both were used as references and neither was copied; provenance is in the README table.
- Synchronous replay. `nvnQueueSubmitCommands` returns when the lists are recorded into Vulkan, not when the GPU has run them. State is therefore always the state at submit time, which is what the console reads. No incarnation or snapshot machinery of the asynchronous designs is needed. Fences are timeline values.
- The shader cache is keyed by the code block only (SHA-1, first 16 hex digits, over the `code_size` bytes at control +0x704). The survey keyed by code plus control section (67,506 blocks); the translated SPIR-V depends on the code alone: 9,750 of the 13,016 distinct code blocks occur with several control sections, and for all 9,750 the SPIR-V, status and resource list are byte-identical (checked on the unpatched run of all 67,506 blocks, `tools/shaders/scripts/make_code_corpus.py --verify`). The runtime has no control length to hash anyway.
- Binding contract. Ryujinx's Vulkan scheme: set 0 uniform buffers (binding 0 is the support buffer, then `stageSlot * 18 + 1 + k` for `c[k]`), set 1 storage buffers (`stageSlot * 16 + k`), set 2 textures (`stageSlot * 128 + i`), set 3 images. `c[0]` is a 64 KiB zero buffer with the maximum point size at 0x610; `c[1]` is the block's constant data; `c[3 + n]` is uniform buffer `n` of the stage; texture handles come from `c[2]` (the `BindTexture` slots) or from a word of a bound uniform buffer. Storage buffer descriptors in `c[0]`: byte offset 0x110 + 0x100 * hardware stage + 0x10 * binding (hardware stage: vertex 0, geometry 3, fragment 4; compute 0x310), confirmed by the offsets the translator reports (0x110, 0x410, 0x510, 0x310).
- Window origin. Upper-left: window y is the image row from the top and NDC +y is the top edge of the viewport rectangle, so the viewport has a negative height and the front face is kept. Lower-left (the GL convention): window y 0 is memory row 0 and NDC -y is the first row, so the viewport is not flipped, the front face is inverted, and the picture is mirrored relative to upper-left. The game uses upper-left.
- API version. The game's own check (`NVNManaged::CDevice::Init`) refuses to start unless `nvnDeviceGetInteger(API_MAJOR_VERSION)` is 55 and `API_MINOR_VERSION` is at least 8 ("NVN API version does not match driver version"). The practice mod's header says 53.105 and smo says 53.312: both would fail. The back end reports 55.15, as Wonder does.

## Shader cache

`work/shader_cache/` (game-derived, never in git):

| Item | Value |
|---|---|
| Distinct blocks (code + control) | 67,506 (33,744 vertex, 33,744 pixel, 12 geometry, 6 compute) |
| Distinct code blocks | 13,016 |
| Translation of all 67,506, unpatched Ryujinx | 67,506 valid, 0 invalid, 0 exceptions, 0 timeouts: 39,082 first try, 28,424 after the DebugMode retry; 900 s with 12 jobs |
| Translation of all 67,506, patched (final) | 67,506 valid, 0 invalid, 0 exceptions, 0 timeouts (39,082 first try, 28,424 after the retry; 980 s with 12 jobs); no block reports the Pixld message; the SPIR-V of all 67,506 blocks is byte-identical to the code-keyed cache, and the 9,750 multi-control groups all agree (`make_code_corpus.py --verify`) |
| Cache the runtime reads | `work/shader_cache/spirv`: 13,016 `.spv` (1,008 MB) and `results.tsv`; 13,016 of 13,016 valid |
| Size options (268-module sample, extrapolated) | raw 1,008 MB; `spirv-opt --strip-debug -O` removes 43 % and still validates (0 failures), 0.09 s per module; with zstd -19 the whole cache would be about 80 MB (the study's 0.37 GB figure is for the 67,506 keyed blocks). The maintainer decides the packaging. |

Rebuild: `python tools\shaders\scripts\make_code_corpus.py work\survey\shaders\corpus --out work\shader_cache\corpus_code`, then `dotnet tools\shaders\build\bin\Release\net10.0\spirv_harness.dll --batch work\shader_cache\corpus_code --out work\shader_cache\spirv --jobs 16 --spirv-val C:\VulkanSDK\1.4.363.0\Bin\spirv-val.exe --target-env vulkan1.3`.

### Changes to the translator (all in `tools/shaders`, never in `refs/`)

| Change | Why |
|---|---|
| `PIXLD` implemented (`spirv-pixld-*.patch`) | See the next section. |
| `gl_PerVertex` declares `ClipDistance[7]` instead of 8 | Ryujinx declares 8 clip + 1 cull distance; NVIDIA and Intel allow 8 combined (`VUID-RuntimeSpirv-maxCombinedClipAndCullDistances-00710`). |
| Host profile: no `SPV_EXT_fragment_shader_interlock` | Every fragment module declared the capability; the shaders that need ordered fragments are rare (none of 500 sampled uses it), and the requirement would exclude GPUs. |

### PIXLD.MY_INDEX

`PIXLD` is the Maxwell instruction that reads pixel and sample information. Its mode field is `PixMode` (COVMASK, COVERED, OFFSET, CENTROID_OFFSET, MY_INDEX). Ryujinx decodes it, logs "Pixld is not implemented" and does not write the destination register, which in SPIR-V is an undefined value. 33,361 of the 33,744 pixel blocks (98.9 %) contain it, always as `PIXLD.MY_INDEX` (mode 5) into R0 near the end of the program.

What MY_INDEX is: the index of the sample the invocation runs for (nouveau and NAK load `gl_SampleID` with this mode). What the epilogue does with it: it is the first term of an index into a byte table at `c[0] + 0x620`: `(sample & mask0) + (((x & mask2) + ((y & mask3) << mask2)) << mask1) + 0x620`, masks and shifts in `c[0] + 0x630`. A position and sample keyed dither table in the driver constants, the shape of an alpha-to-coverage dither. In the 40 pixel shaders the survey inspected, and in the three dumped for this note, the loaded byte reaches the outputs only through paths the shader's own flag disables, and in the simplest shaders the whole chain is dead code, which is why the colour path looked unaffected before the lowering.

Lowering: MY_INDEX reads `gl_SampleID` (BuiltIn SampleId, capability SampleRateShading). On a single-sample target that is 0; on a multisampled target Vulkan runs the shader per sample, as the console does. The other modes write 0 and log. Result: the message disappears from all blocks (0 of 13,016), every module validates, and the 2,848 pixel blocks that read `c[0]` get a defined index. The contents of the `c[0]` dither table are not known; the back end binds zeros, which gives threshold 0 everywhere. Nothing was captured from a console to check the alpha-to-coverage reading.

## Oracles

All run from `runtime\` after `pwsh scripts\build.ps1` (clang-cl, Ninja); environment `NXRT_GFX_HIDDEN=1` keeps windows hidden. Data paths default to `C:\mpr\work\...` and are overridden by `NXRT_NVN_SHADER_CACHE`, `NXRT_NVN_CORPUS`. A group skips when the survey data is absent.

| Group | What it proves |
|---|---|
| `gfx_nvn_abi`, `gfx_nvn_impl` | Thunks marshal integers, enums, bools, pointers, floats and 8-byte structs in both directions; all 513 implementations register with signatures that agree with the generated table. |
| `gfx_nvn_vertex`, `blend_color`, `depth_poly_ms`, `sampler`, `texture_builder`, `builders`, `device_info` | Plain-data families: every Set/Get pair, defaults, `decode()`, zero objects, copies by memcpy. |
| `gfx_nvn_device`, `memory`, `texture`, `program`, `cmd_state`, `cmd_work` | Host objects and recorders without a GPU. |
| `gfx_nvn_enums`, `format`, `texdata`, `shaders` | 156 formats; every enum converter; texel data preparation (untile, BC passthrough, ASTC to BC7 and its cache); all 13,016 cache entries load, hash to their key and reflect to exactly the binding list the translator printed. |
| `gfx_nvn_texgpu`, `present_pass`, `mask_clear` | Vulkan modules against a headless device with validation: uploads read back bit exact, the sampler cache with border colours and anisotropy, present crop and letterbox, masked clears per channel. |
| `gfx_nvn_scene_clear` | Clear through the NVN API, present, read back. |
| `gfx_nvn_scene_triangle` | A triangle with two real translated shaders (position-only vertex shader, constant-colour pixel shader): 384 magenta pixels, apex and base exactly where the geometry says. |
| `gfx_nvn_scene_geometry` (19 cases) | All primitive types including the converted ones (QUADS, QUAD_STRIP, POLYGON, LINE_LOOP), index types, base vertex, instancing with divisor, primitive restart, viewport, scissor, both window origins, both depth modes, culling and winding, polygon modes, state across command buffers. |
| `gfx_nvn_scene_state` (10) | Depth func and write, 8 blend configurations, channel mask, stencil, scissored clear, second render target, integer clear (R32UI read back by copy), polygon offset, logic op, partial-channel colour clear. |
| `gfx_nvn_scene_texture` (17) | Magnification, linear and nearest, wrap modes, border colour, LOD bias and clamp, swizzle, level and sRGB views, BC1, BC7, three real game textures (RGBA8 exact, ASTC 8x5 at 45 dB PSNR against the ASTC decode, BC1 sRGB at 53 dB), render to texture then sample, buffer-to-texture, texture-to-buffer, texture-to-texture (scaled with the linear flag), texel write/read. Cube and array scenes are skipped: no small vertex shader supplies the varyings the cube and array pixel shaders need. |
| `gfx_nvn_scene_commands` (7) | Uniform buffers read by a real shader, GPU-ordered `UpdateUniformBuffer`, handle reuse, `CallCommands`, dropped handles, fences, events, `ReportValue`, `ClearBuffer`, 120 presented frames (0.6 ms of CPU per frame), two queues, window crop and acquire order, a real compute shader. |
| `gfx_nvn_scene_msaa` | A 4x multisampled target resolved by `nvnCommandBufferDownsample`: partial-coverage edge pixels. |

Scenes also write reference images to `work\nvn_refs\` when `NXRT_NVN_UPDATE_REFS=1` and compare against them when the file exists (mean 0.05, max 4 of 255). The references are rendered from game data and are not in git.

## Debug notes

`docs/debug/040.md` to `047.md`.

## Open points

| Gap | Why it matters / state |
|---|---|
| Sparse virtual textures | The game maps virtual tiles (`nvnMemoryPoolMapVirtual`, `nvnTextureBuilderGetSparseTileLayout`, 40-byte `NVNmappingRequest`: physical pool, physical offset, virtual offset, size, storage class). Not implemented: `MapVirtual` returns 0. Needs a Vulkan sparse image or a resident approximation. |
| Transform feedback | `Begin/EndTransformFeedback` and `BindTransformFeedbackBuffer` are called by the game; recorded, not executed. The translator's host profile has transform feedback off. |
| Occlusion counters | `ReportCounter` writes a non-zero count (everything visible) except for timestamps. |
| Integer vertex attributes | The cached SPIR-V declares every vertex input as float. Integer NVN formats are fed through the `*SCALED` Vulkan formats (values converted, not the raw bits the console delivers). A SPIR-V variant per attribute type (Wonder `nvn_spirv_patch.cpp`) is needed if a shader treats the input bits as integers. Logged once as `attr-type-mismatch`. |
| Alpha test | Not applied. The game does not enable it (`ColorStateSetAlphaTest` has no callers). |
| Cube, 2D array and 3D texture scenes with real shaders | Needs a matching vertex shader; image creation and views are covered by `gfx_nvn_texgpu`. |
| Multisample details | Downsample resolves colour only; depth resolve and per-sample shading paths are untested beyond the one scene. |
| Compute inputs | The `c[0]` descriptor table that compute shaders read themselves (`UniformAsStorage`) is not filled; storage buffers through the translator's descriptors work (one compute scene). |
| Shader cache size | 1,008 MB raw for 13,016 modules; see the size options above. |
| Pipeline creation | Synchronous on the render thread; a disk `VkPipelineCache` (`[nvn] pipeline_cache`) softens it. Prewarming from recorded keys is not done. |
| Dynamic resolution crop | `nvnWindowSetCrop` is honoured by the present pass (scaled to the frame). Aspect-correct letterboxing is `BackendDesc::keep_aspect`; a window-size policy is the integration's job. |
| ABI 3 build | This branch was built and tested against the ABI 2 header of recomp commit 5e0fa38 (`-DNXRT_RECOMP_INCLUDE=C:/mpr/work/recomp_abi2/include -DNXRT_BUILD_LIFT_TESTS=OFF`); main has since moved to ABI 3. The branch touches no OS-layer file and merges without conflicts; rebuild after the merge. |

## Second GPU (Intel UHD 770)

`NXRT_GFX_GPU=integrated` runs every scene group on the iGPU. Validation is clean once the Float64 capability that the translator declares in every module is dropped before module creation (no module uses a 64-bit float; the UHD 770 has no `shaderFloat64`). Four cases still differ from the reference images by more than the tolerances, as expected between two rasterizers: `polygon_mode_line_and_point`, `blend_funcs_equations_and_constant` (blend rounding), `bc1_blocks_built_by_hand` and `real_astc_8x5_texture_transcoded` (BC1/BC7 decode). They are tolerance differences, not backend errors, and the RTX 3080 passes all of them.

## Entry-point coverage

513 of 523 NVN entry points have an implementation; 10 are still logging stubs. 204 entry points are called by the game (all implemented).

Status: `real` = implemented as specified; `partial` = implemented but approximated or ignored (note says how); `stub` = still the logging stub. `game` = the game binary calls it.

| Family | Functions | real | partial | stub | Not real or not implemented |
|---|---:|---:|---:|---:|---|
| BlendState | 15 | 15 | 0 | 0 |  |
| Bootstrap | 1 | 1 | 0 | 0 |  |
| Buffer | 11 | 11 | 0 | 0 |  |
| BufferBuilder | 7 | 7 | 0 | 0 |  |
| ChannelMaskState | 3 | 3 | 0 | 0 |  |
| ColorState | 7 | 7 | 0 | 0 |  |
| CommandBuffer | 146 | 108 | 38 | 0 | 38 (listed below) |
| CommandBufferMulti | 2 | 2 | 0 | 0 |  |
| DepthStencilState | 13 | 13 | 0 | 0 |  |
| Device | 27 | 22 | 5 | 0 | DeviceInstallDebugCallback, DeviceRegisterFastClearColor, DeviceRegisterFastClearDepth, DeviceSetIntermediateShaderCache, DeviceWalkDebugDatabase |
| DeviceBuilder | 3 | 3 | 0 | 0 |  |
| Event | 6 | 5 | 0 | 1 | EventGetMemoryPool |
| EventBuilder | 5 | 4 | 0 | 1 | EventBuilderGetStorage |
| LoadCProcs | 1 | 1 | 0 | 0 |  |
| MemoryPool | 10 | 9 | 1 | 0 | MemoryPoolMapVirtual |
| MemoryPoolBuilder | 8 | 8 | 0 | 0 |  |
| MultisampleState | 23 | 23 | 0 | 0 |  |
| PolygonState | 9 | 9 | 0 | 0 |  |
| Program | 7 | 4 | 2 | 1 | ProgramSetSampleShading, ProgramSetShadersExt, ProgramSetSubroutineLinkage |
| Queue | 15 | 14 | 1 | 0 | QueueWaitSync |
| QueueBuilder | 19 | 19 | 0 | 0 |  |
| Sampler | 15 | 15 | 0 | 0 |  |
| SamplerBuilder | 25 | 25 | 0 | 0 |  |
| SamplerPool | 8 | 8 | 0 | 0 |  |
| SetExternalDebuggerEnabledNVX | 1 | 1 | 0 | 0 |  |
| Sync | 6 | 4 | 0 | 2 | SyncCreateGLSync, SyncInitializeFromFencedGLSync |
| Texture | 31 | 29 | 1 | 1 | TextureGetRawStorageClass, TextureGetSparseTileLayout |
| TextureBuilder | 43 | 39 | 1 | 3 | TextureBuilderGetDevice, TextureBuilderGetPackagedTextureLayout, TextureBuilderGetRawStorageClass, TextureBuilderGetSparseTileLayout |
| TexturePool | 8 | 8 | 0 | 0 |  |
| TextureView | 14 | 14 | 0 | 0 |  |
| VertexAttribState | 5 | 5 | 0 | 0 |  |
| VertexStreamState | 5 | 5 | 0 | 0 |  |
| Window | 12 | 12 | 0 | 0 |  |
| WindowBuilder | 12 | 11 | 0 | 1 | WindowBuilderGetNumActiveTextures |

### Partial and stub functions

| Function | Status | Game | What it does |
|---|---|---|---|
| nvnCommandBufferBeginTransformFeedback | partial | game | recorded, not executed (no transform feedback yet) |
| nvnCommandBufferBindSeparateSampler | partial |  | no-op |
| nvnCommandBufferBindSeparateSamplers | partial |  | no-op |
| nvnCommandBufferBindSeparateTexture | partial |  | no-op |
| nvnCommandBufferBindSeparateTextures | partial |  | no-op |
| nvnCommandBufferBindTransformFeedbackBuffer | partial | game | recorded, ignored |
| nvnCommandBufferBindTransformFeedbackBuffers | partial |  | recorded, ignored |
| nvnCommandBufferDownsample | partial | game | colour only (vkCmdResolveImage); depth resolve is not implemented |
| nvnCommandBufferDownsampleTextureView | partial |  | colour only |
| nvnCommandBufferDrawTexture | partial |  | recorded, skipped at replay (counted as unsupported) |
| nvnCommandBufferDrawTransformFeedback | partial |  | no-op |
| nvnCommandBufferEndTransformFeedback | partial | game | recorded, not executed (no transform feedback yet) |
| nvnCommandBufferInsertDebugMarker | partial |  | no-op |
| nvnCommandBufferPauseTransformFeedback | partial |  | no-op |
| nvnCommandBufferPopDebugGroup | partial |  | no-op |
| nvnCommandBufferPushDebugGroup | partial |  | no-op |
| nvnCommandBufferReportCounter | partial | game | timestamp counters are real; occlusion/statistics counters report a non-zero count (everything visible) |
| nvnCommandBufferResetCounter | partial | game | ignored |
| nvnCommandBufferResolveDepthBuffer | partial |  | recorded, skipped |
| nvnCommandBufferRestoreZCullData | partial | game | no-op |
| nvnCommandBufferResumeTransformFeedback | partial |  | no-op |
| nvnCommandBufferSaveZCullData | partial | game | no-op |
| nvnCommandBufferSetAlphaRef | partial |  | stored; the alpha test itself is not applied (the game does not enable it) |
| nvnCommandBufferSetConservativeRasterDilate | partial |  | no-op |
| nvnCommandBufferSetConservativeRasterEnable | partial | game | recorded, ignored |
| nvnCommandBufferSetInnerTessellationLevels | partial |  | recorded, ignored (no tessellation) |
| nvnCommandBufferSetOuterTessellationLevels | partial |  | recorded, ignored (no tessellation) |
| nvnCommandBufferSetPatchSize | partial |  | recorded, ignored (no tessellation) |
| nvnCommandBufferSetPointSize | partial |  | stored; the vertex shader writes the size |
| nvnCommandBufferSetShaderScratchMemory | partial | game | ignored (the GPU manages scratch) |
| nvnCommandBufferSetSubpixelPrecisionBias | partial |  | no-op |
| nvnCommandBufferSetTiledCacheAction | partial | game | no-op |
| nvnCommandBufferSetTiledCacheTileSize | partial |  | no-op |
| nvnCommandBufferSetViewportSwizzles | partial |  | no-op |
| nvnCommandBufferSignalEvent | partial |  | SET and ADD are written into event memory in GPU order; the location argument is ignored |
| nvnCommandBufferTiledDownsample | partial |  | colour only |
| nvnCommandBufferTiledDownsampleTextureView | partial |  | colour only |
| nvnCommandBufferWaitEvent | partial |  | not executed on the GPU (single in-order queue); counted as unsupported |
| nvnDeviceInstallDebugCallback | partial |  | no-op |
| nvnDeviceRegisterFastClearColor | partial | game | accepted, clears are exact anyway |
| nvnDeviceRegisterFastClearDepth | partial | game | accepted |
| nvnDeviceSetIntermediateShaderCache | partial |  | ignored |
| nvnDeviceWalkDebugDatabase | partial |  | no-op |
| nvnEventBuilderGetStorage | stub |  | logging stub |
| nvnEventGetMemoryPool | stub |  | logging stub |
| nvnMemoryPoolMapVirtual | partial | game | returns 0: sparse virtual pools are not implemented (the game calls it for virtual textures) |
| nvnProgramSetSampleShading | partial | game | accepted (SampleId in a shader already forces sample shading) |
| nvnProgramSetShadersExt | stub |  | logging stub |
| nvnProgramSetSubroutineLinkage | partial |  | accepted |
| nvnQueueWaitSync | partial | game | no-op: one in-order GPU queue |
| nvnSyncCreateGLSync | stub |  | logging stub |
| nvnSyncInitializeFromFencedGLSync | stub |  | logging stub |
| nvnTextureBuilderGetDevice | stub |  | logging stub |
| nvnTextureBuilderGetPackagedTextureLayout | stub |  | logging stub |
| nvnTextureBuilderGetRawStorageClass | stub |  | logging stub |
| nvnTextureBuilderGetSparseTileLayout | partial | game | zeros |
| nvnTextureGetRawStorageClass | stub |  | logging stub |
| nvnTextureGetSparseTileLayout | partial |  | zeros |
| nvnWindowBuilderGetNumActiveTextures | stub |  | logging stub |
