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
    std::vector<uint32_t> result;
    result.reserve(in.size() + 4);
    result.insert(result.end(), in.begin(), in.begin() + 5);
    bool changed = false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpDecorate && n.len == 4 && w[2] == DecorationLocation && w[3] == 1 && is_output.count(w[1])) {
            result.insert(result.end(), {(4u << 16) | OpDecorate, w[1], DecorationLocation, 0u});
            result.insert(result.end(), {(4u << 16) | OpDecorate, w[1], 32u /* Index */, 1u});
            changed = true;
            continue;
        }
        result.insert(result.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
    }
    if (!changed) return false;
    out = std::move(result);
    return true;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {
namespace {
enum : uint32_t { OpCapability = 17, OpExecutionMode = 16, OpConstant = 43, OpStore = 62, OpLoad = 61, OpCopyObject = 83, OpAccessChain = 65, OpInBoundsAccessChain = 66,
                  DecOffset = 35, DecXfbBuffer = 36, DecXfbStride = 37, CapTransformFeedback = 53, ExecXfb = 11 };
}  // namespace

bool spirv_add_xfb(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, std::vector<SpvXfbOutput>* layout, uint32_t* stride) {
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3];
    uint32_t f32 = 0, entry_id = 0;
    size_t first_type = 0, after_entry = 0, first_cap_end = 0;
    std::map<uint32_t, uint32_t> location;                            // variable id -> Location
    std::map<uint32_t, uint32_t> output_ptr_type;                     // output variable id -> pointer type id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> vecs, ptrs;     // (component, count) / (storage, pointee) -> id
    std::map<uint32_t, uint32_t> constants;                           // OpConstant id -> low word
    std::map<uint32_t, uint32_t> max_component;                       // variable -> highest constant component index stored (+1)
    std::map<uint32_t, bool> whole;                                   // variable used as a whole (store, load, copy)
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (!first_type && is_type_or_constant(n.op)) first_type = n.at;
        if (n.op == OpCapability) first_cap_end = n.at + n.len;
        if (n.op == OpEntryPoint && !entry_id) entry_id = w[2];
        if (n.op == OpEntryPoint || n.op == OpExecutionMode) after_entry = n.at + n.len;
        if (n.op == OpTypeFloat && n.len >= 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4) vecs[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypePointer && n.len == 4) ptrs[{w[2], w[3]}] = w[1];
        else if (n.op == OpConstant && n.len >= 4) constants[w[2]] = w[3];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpVariable && n.len >= 4 && w[3] == StorageOutput) output_ptr_type[w[2]] = w[1];
    }
    if (!first_type || !first_cap_end || !after_entry || !f32 || !entry_id) return false;
    auto is_captured = [&](uint32_t v) { return location.count(v) && output_ptr_type.count(v); };
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if ((n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && n.len >= 5) {
            const uint32_t base = w[3];
            if (!is_captured(base)) continue;
            auto c = constants.find(w[4]);
            if (c == constants.end()) whole[base] = true;
            else max_component[base] = std::max(max_component[base], c->second + 1);
        } else if (n.op == OpStore && n.len >= 3) {
            if (is_captured(w[1])) whole[w[1]] = true;
        } else if ((n.op == OpLoad || n.op == OpCopyObject) && n.len >= 4) {
            if (is_captured(w[3])) whole[w[3]] = true;
        }
    }
    // the captured variables in Location order, each narrowed to the components that are stored
    std::map<uint32_t, uint32_t> by_location;   // Location -> variable id
    for (const auto& kv : output_ptr_type)
        if (location.count(kv.first)) by_location[location[kv.first]] = kv.first;
    if (by_location.empty()) return false;
    std::vector<uint32_t> decos, decls;
    std::map<uint32_t, uint32_t> new_ptr;       // variable id -> replacement pointer type
    uint32_t offset = 0;
    std::vector<SpvXfbOutput> lay;
    std::vector<std::pair<uint32_t, uint32_t>> decorate;   // variable, byte offset
    for (const auto& kv : by_location) {
        const uint32_t var = kv.second;
        uint32_t ncomp = (whole.count(var) || !max_component.count(var)) ? 4u : std::min(4u, max_component[var]);
        if (ncomp < 2) ncomp = 4;                // a lone scalar component keeps its vec4 (a scalar cannot be indexed)
        if (ncomp != 4) {
            const std::pair<uint32_t, uint32_t> vk = {f32, ncomp};
            uint32_t vid;
            auto vit = vecs.find(vk);
            if (vit != vecs.end()) {
                vid = vit->second;
            } else {
                vid = bound++;
                decls.insert(decls.end(), {(4u << 16) | OpTypeVector, vid, f32, ncomp});
                vecs[vk] = vid;
            }
            const std::pair<uint32_t, uint32_t> pk = {StorageOutput, vid};
            uint32_t pid;
            auto pit = ptrs.find(pk);
            if (pit != ptrs.end()) {
                pid = pit->second;
            } else {
                pid = bound++;
                decls.insert(decls.end(), {(4u << 16) | OpTypePointer, pid, StorageOutput, vid});
                ptrs[pk] = pid;
            }
            new_ptr[var] = pid;
        }
        decorate.push_back({var, offset});
        lay.push_back({kv.first, ncomp, offset});
        offset += 4 * ncomp;
    }
    for (const auto& d : decorate) {
        decos.insert(decos.end(), {(4u << 16) | OpDecorate, d.first, DecXfbBuffer, 0u});
        decos.insert(decos.end(), {(4u << 16) | OpDecorate, d.first, DecXfbStride, offset});
        decos.insert(decos.end(), {(4u << 16) | OpDecorate, d.first, DecOffset, d.second});
    }
    std::vector<uint32_t> res;
    res.reserve(in.size() + decos.size() + decls.size() + 8);
    res.insert(res.end(), in.begin(), in.begin() + 5);
    res[3] = bound;
    bool decls_done = false;
    for (const Inst& n : insts) {
        if (n.at == first_type) res.insert(res.end(), decos.begin(), decos.end());
        // the new vector and pointer types go in front of the first variable that uses one (every type precedes its users)
        if (!decls_done && ((n.op == OpVariable && n.len >= 4 && in[n.at + 3] == StorageOutput && new_ptr.count(in[n.at + 2])) || n.op == OpFunction)) {
            res.insert(res.end(), decls.begin(), decls.end());
            decls_done = true;
        }
        std::vector<uint32_t> copy(in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
        if (n.op == OpVariable && copy.size() >= 4 && copy[3] == StorageOutput) {
            auto it = new_ptr.find(copy[2]);
            if (it != new_ptr.end()) copy[1] = it->second;
        }
        res.insert(res.end(), copy.begin(), copy.end());
        if (n.at + n.len == first_cap_end) res.insert(res.end(), {(2u << 16) | OpCapability, CapTransformFeedback});
        if (n.at + n.len == after_entry) res.insert(res.end(), {(3u << 16) | OpExecutionMode, entry_id, ExecXfb});
    }
    if (!decls_done) return false;
    out = std::move(res);
    if (layout) *layout = lay;
    if (stride) *stride = offset;
    return true;
}

bool spirv_outputs_to_private(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    enum : uint32_t { OpVariableOp = 59, OpDecorateOp = 71, OpEntryPointOp = 15, StoragePrivate = 6, DecIndex = 32, DecComponent = 31, DecBuiltIn = 11 };
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    std::map<uint32_t, bool> out_var, has_location, builtin;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpVariableOp && n.len >= 4 && w[3] == StorageOutput) out_var[w[2]] = true;
        else if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecorationLocation) has_location[w[1]] = true;
        else if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecBuiltIn) builtin[w[1]] = true;
    }
    if (out_var.empty()) return false;
    for (const auto& kv : out_var)
        if (builtin.count(kv.first) || !has_location.count(kv.first)) return false;
    const bool before_14 = in[1] < 0x10400;
    std::vector<uint32_t> res(in.begin(), in.begin() + 5);
    res.reserve(in.size());
    for (const Inst& n : insts) {
        std::vector<uint32_t> c(in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
        if (n.op == OpTypePointer && n.len == 4 && c[2] == StorageOutput) c[2] = StoragePrivate;
        else if (n.op == OpVariableOp && n.len >= 4 && c[3] == StorageOutput) c[3] = StoragePrivate;
        else if (n.op == OpDecorateOp && n.len >= 4 && out_var.count(c[1]) && (c[2] == DecorationLocation || c[2] == DecIndex || c[2] == DecComponent)) continue;
        else if (n.op == OpEntryPointOp && before_14) {
            // OpEntryPoint model id name... interface ids: drop the output variables from the interface
            size_t name_words = 0;
            const char* nm = reinterpret_cast<const char*>(&c[3]);
            const size_t max_chars = (c.size() - 3) * 4;
            size_t chars = 0;
            while (chars < max_chars && nm[chars]) ++chars;
            name_words = chars / 4 + 1;
            const size_t iface = 3 + name_words;
            std::vector<uint32_t> d(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(std::min(iface, c.size())));
            for (size_t i = iface; i < c.size(); ++i)
                if (!out_var.count(c[i])) d.push_back(c[i]);
            d[0] = (static_cast<uint32_t>(d.size()) << 16) | OpEntryPointOp;
            c = d;
        }
        res.insert(res.end(), c.begin(), c.end());
    }
    out = std::move(res);
    return true;
}

}  // namespace nxrt::nvn::be::gpu
