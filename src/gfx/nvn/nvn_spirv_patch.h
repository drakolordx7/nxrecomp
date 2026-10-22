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
//   spirv_add_xfb      a vertex shader that runs with transform feedback: the Location outputs are captured into buffer 0 in Location
//                       order, each narrowed to the components the shader stores (a vec4 whose xyz are written is captured as 12
//                       bytes), tightly packed. The console's layout lives in the driver's compile data; this is the layout the
//                       game's stream-out shaders use (docs/S4e.md).
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

// A fragment shader of a pass without colour attachments: every Location output becomes a private variable (the decorations go, and
// the entry point's interface loses them before SPIR-V 1.4). False (out untouched) when no Location output exists or a built-in output
// (FragDepth, SampleMask) does.
bool spirv_outputs_to_private(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);

struct SpvXfbOutput {
    uint32_t location, components, offset;   // offset in bytes inside a captured vertex
};
// Adds XfbBuffer / XfbStride / Offset decorations, the TransformFeedback capability and the Xfb execution mode. layout receives the
// captured outputs, stride the bytes of one captured vertex. False (out untouched): no Location outputs or an unexpected module.
bool spirv_add_xfb(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, std::vector<SpvXfbOutput>* layout, uint32_t* stride);

}  // namespace nxrt::nvn::be::gpu
