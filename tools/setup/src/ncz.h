// SPDX-License-Identifier: GPL-3.0-or-later
// NCZ: the zstd-compressed NCA used inside NSZ / XCZ dumps (format of the "nsz" project, https://github.com/nicoboss/nsz).
#pragma once
#include "storage.h"

namespace mprs {

// Wraps an NCZ file so that reading it yields the bytes of the original NCA (first 0x4000 bytes as stored, the rest decompressed and
// encrypted again with the section keys that the NCZ's section table carries). Random access: block-compressed NCZ (NCZBLOCK)
// seeks directly; a solid stream restarts from the start when a read goes backwards (reads in the pipeline are sequential).
// Throws Error(Input) for a damaged NCZ, and Error(Input) "this build cannot read NSZ" when built without zstd.
StoragePtr open_ncz(StoragePtr ncz);

}  // namespace mprs
