// SPDX-License-Identifier: GPL-3.0-or-later
// HDR patch of the tone-map fragment shader (hdr_tonemap_patch.h).
#include "extras/hdr_tonemap_patch.h"

#include <cstring>
#include <unordered_map>

#include "nvn/spirv_module.h"

namespace nxrt::extras {
namespace {

using spirv::Inst;

uint32_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

}  // namespace

bool spirv_hdr_tonemap(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 8 || !spirv::is_module(in.data(), in.size())) return false;
    std::vector<Inst> insts;
    if (!spirv::split(in, insts)) return false;
    std::unordered_map<uint32_t, size_t> def;   // result id -> index into insts
    for (size_t k = 0; k < insts.size(); ++k) {
        const Inst& n = insts[k];
        // result id position: types and constants have it at word 1; most value instructions at word 2
        auto result_at = [&](uint32_t idx) { if (idx < n.len) def[in[n.at + idx]] = k; };
        switch (n.op) {
            case spirv::OpExtInstImport: case spirv::OpTypeFloat: case spirv::OpTypeVector: case spirv::OpTypeImage: case spirv::OpTypeSampledImage: result_at(1); break;
            case spirv::OpConstant: case spirv::OpLoad: case spirv::OpVectorShuffle: case spirv::OpCompositeConstruct: case spirv::OpImageSampleImplicitLod: case spirv::OpExtInst: case spirv::OpFMul: result_at(2); break;
            default: break;
        }
    }
    auto get = [&](uint32_t id) -> const Inst* {
        auto it = def.find(id);
        return it == def.end() ? nullptr : &insts[it->second];
    };

    // fragment entry point
    bool fragment = false;
    uint32_t glsl = 0;
    for (const Inst& n : insts) {
        if (n.op == spirv::OpEntryPoint && in[n.at + 1] == spirv::ExecModelFragment) fragment = true;
        if (n.op == spirv::OpExtInstImport && std::memcmp(&in[n.at + 2], "GLSL.std.450", 12) == 0) glsl = in[n.at + 1];
    }
    if (!fragment || !glsl) return false;

    // types: 32-bit float, vec3 of it, the constant 1.0
    uint32_t t_float = 0, t_v3 = 0, c_one = 0;
    for (const Inst& n : insts) if (n.op == spirv::OpTypeFloat && in[n.at + 2] == 32) t_float = in[n.at + 1];
    if (!t_float) return false;
    for (const Inst& n : insts) if (n.op == spirv::OpTypeVector && in[n.at + 2] == t_float && in[n.at + 3] == 3) t_v3 = in[n.at + 1];
    for (const Inst& n : insts) if (n.op == spirv::OpConstant && in[n.at + 1] == t_float && in[n.at + 3] == float_bits(1.0f)) c_one = in[n.at + 2];
    if (!t_v3) return false;

    // the 3D samples in program order
    struct Sample { size_t idx; uint32_t result, coord; };
    std::vector<Sample> s3;
    for (size_t k = 0; k < insts.size(); ++k) {
        const Inst& n = insts[k];
        if (n.op != spirv::OpImageSampleImplicitLod) continue;
        const Inst* si = get(in[n.at + 3]);                 // the sampled image value: an OpLoad of a variable
        if (!si || si->op != spirv::OpLoad) continue;
        const Inst* sty = get(in[si->at + 1]);              // its type: OpTypeSampledImage
        if (!sty || sty->op != spirv::OpTypeSampledImage) continue;
        const Inst* img = get(in[sty->at + 2]);             // OpTypeImage: result, sampled type, dim
        if (!img || img->op != spirv::OpTypeImage || in[img->at + 3] != spirv::Dim3D) continue;
        s3.push_back({k, in[n.at + 2], in[n.at + 4]});
    }
    if (s3.size() != 2) return false;

    // the curve outputs: the coordinate of the first sample is (Fma(y_r, s, b), Fma(y_g, s, b), Fma(y_b, s, b))
    const Inst* cc = get(s3[0].coord);
    if (!cc || cc->op != spirv::OpCompositeConstruct || cc->len != 6) return false;
    uint32_t y[3];
    for (int c = 0; c < 3; ++c) {
        const Inst* f = get(in[cc->at + 3 + c]);
        if (!f || f->op != spirv::OpExtInst || in[f->at + 3] != glsl || in[f->at + 4] != spirv::GlslFma || f->len != 8) return false;
        y[c] = in[f->at + 5];
    }

    // the graded colour: Shuffle(sample B) -> vec3 -> Store
    const uint32_t b_res = s3[1].result;
    const Inst* shuf = nullptr;
    for (const Inst& n : insts)
        if (n.op == spirv::OpVectorShuffle && n.len == 8 && in[n.at + 3] == b_res && in[n.at + 4] == b_res && in[n.at + 5] == 0 && in[n.at + 6] == 1 && in[n.at + 7] == 2 && in[n.at + 1] == t_v3) shuf = &n;
    if (!shuf) return false;
    const uint32_t v3_id = in[shuf->at + 2];
    const Inst* store = nullptr;
    for (size_t k = 0; k < insts.size(); ++k)
        if (insts[k].op == spirv::OpStore && insts[k].at > shuf->at && in[insts[k].at + 2] == v3_id) { store = &insts[k]; break; }
    if (!store) return false;

    // build the new module
    out.clear();
    out.reserve(in.size() + 40);
    uint32_t bound = in[3];
    const uint32_t id_one = c_one ? c_one : bound++;
    uint32_t m[3] = {bound++, bound++, bound++};
    const uint32_t id_vec = bound++, id_mul = bound++;
    out.insert(out.end(), in.begin(), in.begin() + 5);
    out[3] = bound;
    bool const_added = c_one != 0;
    for (const Inst& n : insts) {
        if (!const_added && n.op == spirv::OpFunction) {       // the 1.0 constant goes at the end of the global section, before the first function
            out.push_back((4u << 16) | spirv::OpConstant);
            out.push_back(t_float);
            out.push_back(id_one);
            out.push_back(float_bits(1.0f));
            const_added = true;
        }
        if (&n == store) {
            for (int c = 0; c < 3; ++c) {                // m_c = max(y_c, 1)
                out.push_back((7u << 16) | spirv::OpExtInst);
                out.push_back(t_float);
                out.push_back(m[c]);
                out.push_back(glsl);
                out.push_back(spirv::GlslFMax);
                out.push_back(y[c]);
                out.push_back(id_one);
            }
            out.push_back((6u << 16) | spirv::OpCompositeConstruct);
            out.push_back(t_v3);
            out.push_back(id_vec);
            out.push_back(m[0]);
            out.push_back(m[1]);
            out.push_back(m[2]);
            out.push_back((5u << 16) | spirv::OpFMul);
            out.push_back(t_v3);
            out.push_back(id_mul);
            out.push_back(v3_id);
            out.push_back(id_vec);
            out.push_back(in[n.at]);                    // the store itself, with the new value
            out.push_back(in[n.at + 1]);
            out.push_back(id_mul);
            for (uint32_t w = 3; w < n.len; ++w) out.push_back(in[n.at + w]);
            continue;
        }
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
    }
    return const_added;
}

}  // namespace nxrt::extras
