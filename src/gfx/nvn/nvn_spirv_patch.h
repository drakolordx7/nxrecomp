// SPDX-License-Identifier: GPL-3.0-or-later
// SPIR-V rewrites the back end applies to the translated shaders when it builds a pipeline (render thread). The offline translator
// knows one shader at a time; the console's rules are looser than Vulkan's about what the stages of a pipeline have to agree on.
//
//   spirv_add_outputs   a fragment shader reads varyings (or an indexed array of them) that the stage before it never declared. The
//                       console hands it undefined values; Vulkan wants an output in every slot (VUID-RuntimeSpirv-OpEntryPoint-08743).
//                       The producer gets unwritten outputs there.
//   spirv_dual_source_output
//                       a fragment shader of a draw with dual-source blending: the console takes the second blend source from render
//                       target 1's output; Vulkan from the output at Location 0, Index 1 (and allows a single such attachment).
//
// The idea (pattern-matching on the module's declarations, new ids from the bound) follows SuperMarioWonderRecomp's
// nvn_spirv_patch.cpp (MIT); the code is new.
#pragma once
#include <cstdint>
#include <vector>

namespace nxrt::nvn::be::gpu {

enum class SpvScalar : uint8_t { Float, Sint, Uint };

struct SpvOutputSlot {
    uint32_t location = 0;
    SpvScalar type = SpvScalar::Float;
};

// Adds one never-written vec4 Output variable per slot (a 4-component vector of the slot's scalar type) to the entry point of `in`.
// Returns false (out untouched) when the module cannot be rewritten (malformed, no entry point, a slot that already has an output).
bool spirv_add_outputs(const std::vector<uint32_t>& in, const std::vector<SpvOutputSlot>& slots, std::vector<uint32_t>& out);

// Moves the fragment output at Location 1 to Location 0, Index 1. False (out untouched) when the module has no such output.
bool spirv_dual_source_output(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);

}  // namespace nxrt::nvn::be::gpu
