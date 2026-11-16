// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to src/os/posix: the unwinder entry that thread.cpp (the sampling signal handler) and symbols.cpp share.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::os::detail {

// Walks the stack of the calling thread with libgcc's unwinder (.eh_frame) from inside a signal handler: the frames of the handler and of
// the signal trampoline are dropped, the walk records from the frame whose address is `match_pc` (the interrupted instruction) outwards.
// Returns the number of frames written (0 when match_pc was never met). Does not allocate and takes no lock of its own (glibc 2.35+ finds the
// unwind tables without one).
size_t unwind_from_handler(uint64_t match_pc, uint64_t* frames, size_t max);

}  // namespace nxrt::os::detail
