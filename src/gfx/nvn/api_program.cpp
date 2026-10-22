// SPDX-License-Identifier: GPL-3.0-or-later
// NVNprogram: the host Program object (nvn_core.h) behind the 0xC0-byte guest object, and the entry points that set its
// stages. SetShaders takes each NVNshaderData{data, control}: data is the GPU address of the code block (a host pointer into
// the game's pool), control the control section. The shader cache (nvn_shaders.h) gives the ShaderInfo of the block; its
// NVN stage comes from the SPIR-V execution model. A block the cache cannot translate leaves the program unlinked.
//
// Signatures: the five entries in work/nvn/sigs.txt use those types. nvnProgramSetSampleShading has no signature in sigs.txt
// (table entry '?'): assumed void(NVNprogram*, int), the argument order smo-recomp runtime/gfx/nvn_gl.c (GPL-3.0) uses
// (layout/defaults checked, no code copied). nvnProgramSetShadersExt is not implemented: its stride is not settled (see
// the report). nvnProgramSetSubroutineLinkage is accepted and ignored: the back end has no subroutine support.
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

#include "nvn_core.h"
#include "nvn_impl.h"
#include "nvn_shaders.h"
#include "nxrt/log.h"

namespace nxrt::nvn {
using namespace be;
namespace {

static_assert(sizeof(GuestHeader) <= sizeof(NVNprogram), "program header fits the NVN object");

// Blocks larger than this are not hashed for a log key (a corrupt control section must not make us read far).
constexpr uint32_t kMaxLogHashBytes = 16u << 20;

// NVN stage index of a translated stage (NVNshaderStage values); -1 for stages the back end cannot run (tessellation).
int nvn_stage_of(ShaderStage s) {
    switch (s) {
        case ShaderStage::Vertex: return 0;
        case ShaderStage::Fragment: return 1;
        case ShaderStage::Geometry: return 2;
        case ShaderStage::Compute: return 5;
        default: return -1;
    }
}

// The 16 hex digits of a shader hash as a number. 0 if the text is not hex.
uint64_t parse_hash(const std::string& hex) {
    uint64_t v = 0;
    for (char ch : hex) {
        uint64_t d;
        if (ch >= '0' && ch <= '9') d = static_cast<uint64_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<uint64_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = static_cast<uint64_t>(ch - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    return v;
}

// True the first time a key is seen: the log line of a missing shader is written once per hash.
bool first_report(const std::string& key) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lk(mu);
    return seen.insert(key).second;
}

// The key of a block the cache could not load: its hash when the control section is sane, else the control address.
std::string missing_key(const uint8_t* code, const uint8_t* control) {
    const ShaderControl c = read_control(control);
    if (c.valid && c.code_size != 0 && c.code_size <= kMaxLogHashBytes) return shader_hash(code, c.code_size);
    char buf[40];
    std::snprintf(buf, sizeof buf, "control@%p", static_cast<const void*>(control));
    return buf;
}

void report_missing(const uint8_t* code, const uint8_t* control) {
    if (!code || !control) {
        if (first_report("null")) NXLOG_W("nvn", "nvnProgramSetShaders: a stage has no code or control address; program not linked");
        return;
    }
    const std::string key = missing_key(code, control);
    if (first_report(key)) NXLOG_W("nvn", "nvnProgramSetShaders: no translated shader for block %s; program not linked", key.c_str());
}

NVNboolean nvnProgramInitialize(NVNprogram* program, NVNdevice* device) {
    (void)device;   // the program needs no device state of its own
    if (!program) return 0;
    bind_guest(program, new Program());
    return 1;
}

void nvnProgramFinalize(NVNprogram* program) {
    if (!host_of<Program>(program)) return;
    unbind_guest(program);
}

void nvnProgramSetDebugLabel(NVNprogram* program, const char* label) {
    if (Program* p = host_of<Program>(program)) p->label = label ? label : "";
}

NVNboolean nvnProgramSetShaders(NVNprogram* program, int count, const NVNshaderData* shaders) {
    Program* p = host_of<Program>(program);
    if (!p) return 0;
    for (int s = 0; s < 6; ++s) {
        p->stage[s] = nullptr;
        p->code_hash[s] = 0;
    }
    ++p->serial;
    p->linked = false;
    if (count > 0 && !shaders) return 0;

    bool all_found = true;
    for (int k = 0; k < count; ++k) {
        const uint8_t* code = host_ptr(shaders[k].data);
        const uint8_t* control = static_cast<const uint8_t*>(shaders[k].control);
        const ShaderInfo* si = (code && control) ? shader_cache().find(code, control) : nullptr;
        if (!si) {
            report_missing(code, control);
            all_found = false;
            continue;
        }
        const int stage = nvn_stage_of(si->stage);
        if (stage < 0) {
            if (first_report("stage:" + si->hash))
                NXLOG_W("nvn", "nvnProgramSetShaders: block %s is a stage the back end does not run; program not linked", si->hash.c_str());
            all_found = false;
            continue;
        }
        p->stage[stage] = si;
        p->code_hash[stage] = parse_hash(si->hash);
    }
    p->linked = all_found;
    return all_found ? 1 : 0;
}

NVNboolean nvnProgramSetSubroutineLinkage(NVNprogram* program, int stage, const NVNsubroutineLinkageMapPtr* map) {
    (void)stage;
    (void)map;
    if (!host_of<Program>(program)) return 0;
    if (first_report("subroutine-linkage")) NXLOG_W("nvn", "nvnProgramSetSubroutineLinkage: subroutines are not supported; the linkage is ignored");
    return 1;
}

// Assumed signature (see the header comment). The back end keeps no sample-shading state yet, so this only validates.
void nvnProgramSetSampleShading(NVNprogram* program, int mode) {
    (void)mode;
    if (!host_of<Program>(program)) return;
}

}  // namespace

void register_api_program() {
    NVN_REG(nvnProgramInitialize);
    NVN_REG(nvnProgramFinalize);
    NVN_REG(nvnProgramSetDebugLabel);
    NVN_REG(nvnProgramSetShaders);
    NVN_REG(nvnProgramSetSubroutineLinkage);
    NVN_REG(nvnProgramSetSampleShading);
}

}  // namespace nxrt::nvn
