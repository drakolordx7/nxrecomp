// SPDX-License-Identifier: GPL-3.0-or-later
// SPIR-V rewrites (declarations in nvn_spirv_patch.h).
#include "nvn_spirv_patch.h"

#include <cstring>
#include <map>

namespace nxrt::nvn::be::gpu {
namespace {

enum : uint32_t {
    OpEntryPoint = 15, OpTypeVoid = 19, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpTypeForwardPointer = 39,
    OpFunction = 54, OpVariable = 59, OpDecorate = 71,
    DecorationLocation = 30, StorageOutput = 3,
};

struct Inst {
    size_t at;
    uint32_t op, len;
};

bool is_type_or_constant(uint32_t op) { return (op >= OpTypeVoid && op <= OpTypeForwardPointer) || (op >= 41 && op <= 52); }

}  // namespace

bool spirv_add_outputs(const std::vector<uint32_t>& in, const std::vector<SpvOutputSlot>& slots, std::vector<uint32_t>& out) {
    if (slots.empty() || in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }

    uint32_t bound = in[3];
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ints;    // (width, signed) -> id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> vecs;    // (component type, count) -> id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ptrs;    // (storage class, pointee) -> id
    std::map<uint32_t, uint32_t> output_location;              // variable id -> Location
    std::map<uint32_t, uint32_t> output_vars;                  // output variable id -> pointee type id
    uint32_t f32 = 0;
    size_t first_type = 0, first_function = 0, entry_at = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (!first_type && is_type_or_constant(n.op)) first_type = n.at;
        if (!first_function && n.op == OpFunction) first_function = n.at;
        if (n.op == OpEntryPoint && !entry_at) entry_at = n.at;
        if (n.op == OpTypeInt && n.len == 4) ints[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypeFloat && n.len >= 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4) vecs[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypePointer && n.len == 4) ptrs[{w[2], w[3]}] = w[1];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) output_location[w[1]] = w[3];
    }
    if (!first_type || !first_function || !entry_at || !f32) return false;
    for (const Inst& n : insts)
        if (n.op == OpVariable && n.len >= 4 && in[n.at + 3] == StorageOutput) output_vars[in[n.at + 2]] = in[n.at + 1];
    for (const SpvOutputSlot& s : slots)
        for (const auto& kv : output_location)
            if (output_vars.count(kv.first) && kv.second == s.location) return false;   // already declared (as a single variable)

    std::vector<uint32_t> decos, decls, new_vars;
    auto scalar_id = [&](SpvScalar t) -> uint32_t {
        if (t == SpvScalar::Float) return f32;
        const std::pair<uint32_t, uint32_t> k = {32, t == SpvScalar::Sint ? 1u : 0u};
        auto it = ints.find(k);
        if (it != ints.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpTypeInt, id, 32u, k.second});
        ints[k] = id;
        return id;
    };
    auto vec4_id = [&](uint32_t scalar) -> uint32_t {
        const std::pair<uint32_t, uint32_t> k = {scalar, 4};
        auto it = vecs.find(k);
        if (it != vecs.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpTypeVector, id, scalar, 4u});
        vecs[k] = id;
        return id;
    };
    auto out_ptr_id = [&](uint32_t pointee) -> uint32_t {
        const std::pair<uint32_t, uint32_t> k = {StorageOutput, pointee};
        auto it = ptrs.find(k);
        if (it != ptrs.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpTypePointer, id, StorageOutput, pointee});
        ptrs[k] = id;
        return id;
    };
    for (const SpvOutputSlot& s : slots) {
        const uint32_t ptr = out_ptr_id(vec4_id(scalar_id(s.type)));
        const uint32_t var = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpVariable, ptr, var, StorageOutput});
        decos.insert(decos.end(), {(4u << 16) | OpDecorate, var, DecorationLocation, s.location});
        new_vars.push_back(var);
    }

    out.clear();
    out.reserve(in.size() + decos.size() + decls.size() + new_vars.size() + 4);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    out[3] = bound;
    const Inst* entry = nullptr;
    for (const Inst& n : insts)
        if (n.at == entry_at) entry = &n;
    for (const Inst& n : insts) {
        if (n.at == first_type) out.insert(out.end(), decos.begin(), decos.end());
        if (n.at == first_function) out.insert(out.end(), decls.begin(), decls.end());
        if (&n == entry) {
            // the interface list is the tail of OpEntryPoint
            out.push_back(((n.len + static_cast<uint32_t>(new_vars.size())) << 16) | OpEntryPoint);
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at) + 1, in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
            out.insert(out.end(), new_vars.begin(), new_vars.end());
            continue;
        }
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
    }
    return true;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {

bool spirv_dual_source_output(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    std::map<uint32_t, bool> is_output;
    for (const Inst& n : insts)
        if (n.op == OpVariable && n.len >= 4 && in[n.at + 3] == StorageOutput) is_output[in[n.at + 2]] = true;
    out.clear();
    out.reserve(in.size() + 4);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    bool changed = false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpDecorate && n.len == 4 && w[2] == DecorationLocation && w[3] == 1 && is_output.count(w[1])) {
            out.insert(out.end(), {(4u << 16) | OpDecorate, w[1], DecorationLocation, 0u});
            out.insert(out.end(), {(4u << 16) | OpDecorate, w[1], 32u /* Index */, 1u});
            changed = true;
            continue;
        }
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
    }
    return changed;
}

}  // namespace nxrt::nvn::be::gpu
