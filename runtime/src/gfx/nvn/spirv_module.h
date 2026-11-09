// SPDX-License-Identifier: GPL-3.0-or-later
// The one SPIR-V module walker of the runtime: the header check, the instruction split and the enumerants that shader reflection
// (nvn_shaders.cpp), the pipeline rewrites (nvn_spirv_patch.cpp) and the HDR tone-map patch (extras/hdr_tonemap_patch.cpp) read.
// Values are from the SPIR-V 1.x unified specification and the GLSL.std.450 extended instruction set; only the ones in use are listed.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::spirv {

constexpr uint32_t kMagic = 0x07230203u;
constexpr size_t kHeaderWords = 5;   // magic, version, generator, id bound, schema

enum : uint32_t {   // opcodes
    OpName = 5, OpExtInstImport = 11, OpExtInst = 12, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17, OpTypeVoid = 19,
    OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24, OpTypeImage = 25, OpTypeSampler = 26,
    OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpTypeForwardPointer = 39,
    OpConstant = 43, OpFunction = 54, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66,
    OpDecorate = 71, OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80, OpCopyObject = 83, OpImageSampleImplicitLod = 87,
    OpBitcast = 124, OpFMul = 133, OpEmitVertex = 218, OpLabel = 248,
};
enum : uint32_t {   // decorations
    DecBlock = 2, DecBufferBlock = 3, DecBuiltIn = 11, DecLocation = 30, DecComponent = 31, DecIndex = 32, DecBinding = 33,
    DecDescriptorSet = 34, DecOffset = 35, DecXfbBuffer = 36, DecXfbStride = 37,
};
enum : uint32_t {   // storage classes
    ScUniformConstant = 0, ScInput = 1, ScUniform = 2, ScOutput = 3, ScPrivate = 6, ScStorageBuffer = 12,
};
enum : uint32_t {   // execution models, execution modes, capabilities, image dimensions
    ExecModelVertex = 0, ExecModelGeometry = 3, ExecModelFragment = 4, ExecModelCompute = 5,
    ExecXfb = 11, ExecLocalSize = 17, ExecInputTriangles = 22,
    CapTransformFeedback = 53,
    Dim3D = 2, DimBuffer = 5,
};
enum : uint32_t {   // GLSL.std.450
    GlslFMax = 40, GlslFma = 50,
};

// One instruction: `at` is the index of its first word (opcode and word count), `len` its word count.
struct Inst {
    size_t at;
    uint32_t op, len;
};

// True when the words start with a SPIR-V header (magic and room for the five header words).
inline bool is_module(const uint32_t* words, size_t count) { return count >= kHeaderWords && words[0] == kMagic; }

// Splits the instruction stream after the header into `out`. False when an instruction has word count 0 or runs past the end
// (out then holds the instructions before it). Does not check the magic: the caller decides whether that matters.
bool split(const uint32_t* words, size_t count, std::vector<Inst>& out);
inline bool split(const std::vector<uint32_t>& m, std::vector<Inst>& out) { return split(m.data(), m.size(), out); }

// A literal string operand (nul-terminated, packed little-endian into words). used receives the words it takes.
std::string literal_string(const uint32_t* w, size_t n, size_t* used);

// Type declarations and constants: the block of the module new types and constants are inserted into.
inline bool is_type_or_constant(uint32_t op) { return (op >= OpTypeVoid && op <= OpTypeForwardPointer) || (op >= 41 && op <= 52); }

}  // namespace nxrt::spirv
