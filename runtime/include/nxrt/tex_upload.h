// SPDX-License-Identifier: GPL-3.0-or-later
// Texture upload hook (S17, S45): lets the host change the pixels of game textures as the runtime uploads them. It replaces the two older
// hooks (a whole small BC7 icon, and the per-level edit of ASTC textures) with one list of subscribers; each subscriber says which textures it
// wants and what to do with them, and the runtime does the decode and the re-encode.
//
// Used by the button prompts (src/gfx/play/prompts.cpp: the game's UI icons become Xbox, PlayStation or keyboard and mouse icons) and by a game's own
// texture tweaks (a game module: recolouring suit textures, say).
//
// What the runtime offers a subscriber (src/gfx/nvn/nvn_tex_upload.cpp):
//   * ASTC 2D textures with one layer (decoded and edited at every level, re-encoded to BC7 or RGBA8): `match` sees the texture, `edit` gets each level of the
//     game's own image as RGBA8 (astc = true in the request). The result is made once per (id, generation) and kept in memory, so uploading the same
//     texture again costs a copy. It never goes into the BC7 disk cache, which stays the game's own look.
//   * single-level BC7 sRGB textures of at most 128 x 128 (astc = false): the subscriber supplies the whole image (the runtime has no BC7 decoder, so
//     `edit` gets zeroed pixels); it is encoded to BC7 at the same size.
//   * texture_upload_changed (after a subscriber's answers changed, e.g. a new colour): a worker thread makes the new data of every live texture
//     that matches (or was edited before) and asks for each to be uploaded again once its data is ready, so the render thread does not wait for the encode.
// The subscribers are asked in the order they subscribed; the first non-zero `match` wins and its id is kept apart from the others' (the runtime
// puts the subscriber's slot in the top byte, so ids must be below 2^56). All callbacks may run on any thread (render thread, background texture
// jobs, the worker) and must be thread safe.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::gfx {

struct TexUploadReq {
    uint32_t width = 0, height = 0;   // level 0, texels
    uint32_t levels = 0;
    bool astc = false;                // the game's texture is ASTC (block_w x block_h); false: a single-level BC7 sRGB texture
    uint32_t block_w = 0, block_h = 0;   // ASTC block footprint (0 when not ASTC)
    bool srgb = false;
    const uint8_t* tiled = nullptr;   // the texture's storage in the game's memory exactly as the game wrote it (block-linear)
    size_t tiled_size = 0;
};

struct TexUploadHooks {
    // Cheap: the subscriber's id of this texture when it wants to change it now, 0 to leave it alone.
    uint64_t (*match)(const TexUploadReq& req, void* user) = nullptr;
    // The answer generation: data made under another generation is not used again. Null: always 0.
    uint64_t (*generation)(void* user) = nullptr;
    // Changes one level: w x h straight-alpha RGBA8, sRGB-encoded when the texture is sRGB, rows as stored in the game's memory (a game that keeps its UI
    // textures upside down has row 0 as the bottom row of the picture). ASTC: the game's decoded level, edited in place; called for level 0 first,
    // then 1, 2, ... of the same texture, on one thread. BC7: zeroed, to be filled whole. Returns false to leave the texture as the game's (BC7 only;
    // an ASTC level that returns false stays as decoded).
    bool (*edit)(uint64_t id, uint32_t level, uint32_t w, uint32_t h, uint8_t* rgba, void* user) = nullptr;
    void* user = nullptr;
};

// Adds a subscriber (the hooks are copied; `match` is required): its textures are uploaded again. Returns a handle (> 0), or 0 when there is no slot
// (at most 255 subscribers at a time).
int texture_upload_subscribe(const TexUploadHooks& hooks);
// Removes a subscriber (0 or an unknown handle: nothing): the textures it changed are uploaded again as the game's.
void texture_upload_unsubscribe(int handle);
// The answers changed: the matching textures (and those changed before) get new data in the background and are then uploaded again. Any thread.
void texture_upload_changed();

struct TexUploadStats {
    uint64_t edits = 0;          // edited data made (any thread)
    uint64_t reuses = 0;         // uploads served from the kept data
    uint64_t rebuilds = 0;       // texture_upload_changed passes finished
    double last_edit_ms = 0;     // the last edit: decode, subscriber's edit, encode of the whole chain
    double last_rebuild_ms = 0;  // the last pass of texture_upload_changed: all textures, before they are uploaded again
    uint32_t last_rebuild_textures = 0;
};
TexUploadStats texture_upload_stats();

}  // namespace nxrt::gfx
