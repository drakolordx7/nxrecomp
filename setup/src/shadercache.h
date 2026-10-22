// SPDX-License-Identifier: GPL-3.0-or-later
// Installs the translated-shader cache that ships with the release (13,016 SPIR-V modules + results.tsv, produced by
// tools/shaders; layout read by runtime/src/gfx/nvn/nvn_shaders.cpp: <dir>/results.tsv and <dir>/<16 hex digits>.spv, where the
// 16 digits are the first 16 hex digits of the SHA-1 of the shader's code block).
// The source is either a folder with that layout or a single pack file (see below). The cache is NOT derived from the player's
// dump; it only has to be copied/unpacked and checked.
#pragma once
#include <functional>
#include <string>

#include "common.h"

namespace mprs {

// Pack file format "MPRSHC1" (written by tools/setup/scripts/make_shader_pack.py, read here):
//   magic "MPRSHC01" (8 bytes), u32 entry_count, u32 reserved, then entry_count records {u16 name_len, name bytes (a file name
//   such as "00004b0d6e7742b4.spv" or "results.tsv"), u64 data_offset (from file start), u64 stored_size, u64 raw_size, u8[32] sha256
//   of the RAW bytes, u8 method (0 = stored, 1 = zstd)}, then the data. All integers little-endian. Names must not contain
//   '/', '\\' or ':' .
// A folder source may additionally contain "manifest.sha256" (lines "<64 hex>  <file name>") which is checked when present.

struct ShaderInstallResult {
    u32 files_total = 0, files_installed = 0, files_skipped = 0;   // skipped = already present with the right size/hash
    u64 bytes_written = 0;
    bool had_manifest = false;     // hashes were available and checked
};

// Installs into dest_dir (created). Idempotent: a file that exists with the expected size (and, if verify, hash) is skipped; files
// are written under a ".part" name and renamed. Throws Error(Input) for a source that is not a shader cache (no results.tsv,
// bad magic, hash mismatch, a name that is not "results.tsv" or 16 lower-case hex digits + ".spv"), Error(Disk) for write failures,
// Error(Cancelled) when cancelled() returns true. progress(done, total) counts files.
ShaderInstallResult install_shader_cache(const std::string& src_utf8, const std::string& dest_dir_utf8, bool verify,
                                         const std::function<void(u64 done, u64 total)>& progress,
                                         const std::function<bool()>& cancelled);

}  // namespace mprs
