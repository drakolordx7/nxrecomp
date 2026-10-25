# S4f log: rendering fidelity in gameplay

Branch `nvn3` (worktrees under `runtime-wt/`). The game is run headless-to-window by `mpr.exe` from a private game build (`game-build-nvn3`); references are Eden captures in `work/eden_ref/session7` (the whole opening cutscene every 30 presents, then the hangar).

## What was wrong and what fixed it

| Symptom | Mechanism | Fix |
|---|---|---|
| Rainbow garbage on hull, gunship, platform that blinks between garbage and correct (cutscene) | The game makes several texture objects over one piece of memory: eight objects, one per mip level of a 128x128 environment cube map, are rendered to, and another object over the same bytes is sampled by the materials. Each object had its own Vulkan image, so the sampled image never received the rendered mips and was filled from the stale bytes of the game memory (other textures' data, different every time the memory was reused). | Objects whose storage starts at the same address and has the same shape share one image (`texture_registry_find_alias`, reference counted `TexGpu`). |
| Gunship, Samus' arm and other dynamic objects black | The lighting of dynamic objects samples six sparse 3D probe volumes (512x256x368). The game maps 64 KiB tiles of a physical pool into the virtual pool (`nvnMemoryPoolMapVirtual`); our call did nothing and the volumes stayed empty. | Mappings are recorded per virtual pool; a sparse texture is built from the mapped tiles (`texture_gather_virtual`), is marked dirty when tiles are remapped or the physical memory is flushed. The layout of a sparse volume follows its 64 KiB tile (block 2 x 16 GOBs, size rounded to tiles). |
| Skinned meshes wrong: first-person arm cannon at the bottom, torn character models | Vertex shaders bitcast their integer attribute (RGBA8UI bone indices) back to int. We fed integer formats as scaled floats (3 became 3.0), so every bone but 0 was wrong. | `spirv_int_attributes`: the input becomes an integer vector, the entry point copies it into the old variable with an OpBitcast; the pipeline uses the true integer format. Stream-out draws keep the scaled path. |
| Layered render targets | An array or cube target set without a view was attached with layer 0 only, so a geometry shader's `gl_Layer` writes were lost. | Such targets are attached with all their layers; the pass layer count is the common count. |
| Passthrough geometry shader | A console passthrough geometry shader (80271a63) only sets position and layer; its translation wrote no Location output and the fragment shader read undefined values. | `spirv_gs_passthrough` copies the input attributes (triangle in, three vertices out). |
| Stream-out counters 13 and 14 | Reported 1. | Counted on the CPU per draw (`primitives_for`), 14 only inside a stream-out section; reset by `ResetCounter`. Indirect draws are not counted. |
| Missing shader block f8e9efd694abe636 | Not in the cache. | Translated, validated, added to `work/shader_cache/spirv` (13,019 entries). |
| Dev path in the shader cache search | A fixed path of the author's project folder was a default for the shader cache search. | Removed; defaults are `./shader_cache/spirv` and `<exe folder>/shader_cache/spirv`. |

## Texture cache entries (for the setup tool)

The cache file of an ASTC texture is `<cache>/<2 hex>/<16 hex key>.bc7`, key = hash of the source bytes seeded with the format, size, layout and quality (unchanged). The header (magic `NXB7`, version 1, key, VkFormat, levels, layers, per-level width/height/depth/offset/size) now may declare one of:

* `VK_FORMAT_BC7_UNORM_BLOCK` (145) or `_SRGB` (146): as before.
* `VK_FORMAT_R8G8B8A8_UNORM` (37) or `_SRGB` (43), matching the ASTC format's sRGB-ness: tight RGBA8 texels, levels back to back, all layers of a level together. For a texture BC7 cannot reproduce (the white-noise lookup texture).
* `VK_FORMAT_BC5_UNORM_BLOCK` (141), only for non-sRGB sources: 16-byte 4x4 blocks, the same level geometry as BC7. Sampling gives (R, G, 0, 1): two-channel normal maps.

The runtime reads the header first (`cache_entry_format`), makes the Vulkan image in that format and then delivers the entry's data. The runtime never overwrites an entry of another format. A BC5 entry that disappears makes the texture fail (nothing can recreate it from ASTC).

## Tools

`[nvn]` keys, all environment-overridable: `shot_frames` (numbered PNGs of chosen presents), `trace_frame` (one or several frames), `trace_dump_tex` (prepared and GPU data of sampled textures), `trace_tex_events`. The trace shows sampler, view, memory address, viewport, render target memory and the first indexed vertices. The local script index lists the scripts (`bnrun.sh`, `align_frames.py`, `bc6view.py`).

## Tests

New groups or cases: `gfx_nvn_prim_counters`, `gfx_nvn_layered_target`, `gfx_nvn_spirv_patch` (passthrough, integer attributes), `gfx_nvn_texdata` / `gfx_nvn_texgpu` (RGBA8 and BC5 entries, sparse tiles), `gfx_nvn_memory` (mapped tiles), `gfx_nvn_texture` (sparse layout). The gfx groups are in `nxrt_gfx_tests.exe`.

## Open

* First-person arm cannon: the integer-attribute fix should place it; the last run reached the cutscene Samus (correct, lit and skinned) but the first-person frame was not checked before the session ended. Look at frame 9300 onward in `work/run_nvn3/shots`.
* Engine fire jets of the gunship: checked at 60 and 240 steps per second and in an ultrawide window, no offset from the nozzles found (debug note 445).
* The black spike below the gunship: not isolated (it may have been a skinned-mesh artefact; look again).
* Eden comparison metrics: the Eden capture and `align_frames.py` are ready; the dense run for numbers was not done.
* The sweep of the first rooms against Eden (needs stick input on both sides).
* Validation was not rerun after the last changes (integer attributes, sparse tiles); run once with `NXRT_GFX_VALIDATION=1`.
