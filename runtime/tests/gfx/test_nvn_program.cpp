// SPDX-License-Identifier: GPL-3.0-or-later
// NVNprogram entry points through the typed thunks: Initialize/Finalize binding, the debug label, and the accepted-and-ignored entries. The
// SetShaders cases against a shader corpus are the game's (its tests/test_nvn_program_cache.cpp).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nvn_core.h"
#include "nvn_impl.h"
#include "nvn_shaders.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_program)

using namespace nxrt;
using namespace nxrt::nvn;
using namespace nxrt::nvn::be;   // ShaderStage, shader_cache(), shader_hash() (nvn_shaders.h)

namespace {

const char* kNames[] = {
    "nvnProgramInitialize",   "nvnProgramFinalize",          "nvnProgramSetDebugLabel",
    "nvnProgramSetShaders",   "nvnProgramSetSubroutineLinkage", "nvnProgramSetSampleShading",
};

// nvnProgramSetSampleShading has no typedef in the NVN header; this is the assumed signature (see api_program.cpp).
using PFNSAMPLESHADINGASSUMED = void (*)(NVNprogram*, int);

void setup() {
    install();
    abi::reset_mismatches();
    register_api_program();
    CHECK_EQ(abi::mismatches(), 0u);
}

}  // namespace


NXTEST(entry_points_are_registered) {
    setup();
    for (const char* name : kNames) CHECK_MSG(is_implemented(name), name);
}

NXTEST(initialize_binds_and_finalize_releases) {
    setup();
    auto init = abi::proc<PFNNVNPROGRAMINITIALIZEPROC>("nvnProgramInitialize");
    auto fin = abi::proc<PFNNVNPROGRAMFINALIZEPROC>("nvnProgramFinalize");
    NVNprogram prog;
    std::memset(&prog, 0, sizeof prog);
    CHECK_EQ(init(nullptr, nullptr), 0);
    CHECK_EQ(init(&prog, nullptr), 1);
    be::Program* hp = be::host_of<be::Program>(&prog);
    CHECK(hp != nullptr);
    CHECK(hp && !hp->linked);
    CHECK(hp && hp->serial == 0u);
    fin(&prog);
    CHECK(be::host_of<be::Program>(&prog) == nullptr);
    fin(nullptr);   // no crash
}

NXTEST(debug_label_is_stored_and_null_clears_it) {
    setup();
    auto init = abi::proc<PFNNVNPROGRAMINITIALIZEPROC>("nvnProgramInitialize");
    auto label = abi::proc<PFNNVNPROGRAMSETDEBUGLABELPROC>("nvnProgramSetDebugLabel");
    auto fin = abi::proc<PFNNVNPROGRAMFINALIZEPROC>("nvnProgramFinalize");
    NVNprogram prog;
    std::memset(&prog, 0, sizeof prog);
    init(&prog, nullptr);
    label(&prog, "terrain fragment");
    CHECK(be::host_of<be::Program>(&prog)->label == "terrain fragment");
    label(&prog, nullptr);
    CHECK(be::host_of<be::Program>(&prog)->label.empty());
    fin(&prog);
}

NXTEST(not_ours_objects_are_tolerated) {
    setup();
    auto fin = abi::proc<PFNNVNPROGRAMFINALIZEPROC>("nvnProgramFinalize");
    auto label = abi::proc<PFNNVNPROGRAMSETDEBUGLABELPROC>("nvnProgramSetDebugLabel");
    auto set_shaders = abi::proc<PFNNVNPROGRAMSETSHADERSPROC>("nvnProgramSetShaders");
    auto set_sub = abi::proc<PFNNVNPROGRAMSETSUBROUTINELINKAGEPROC>("nvnProgramSetSubroutineLinkage");
    auto set_shade = abi::proc<PFNSAMPLESHADINGASSUMED>("nvnProgramSetSampleShading");
    NVNprogram prog;
    std::memset(&prog, 0xAB, sizeof prog);   // not a program of ours: no guest header magic
    fin(&prog);
    label(&prog, "x");
    NVNshaderData d{};
    CHECK_EQ(set_shaders(&prog, 1, &d), 0);
    CHECK_EQ(set_sub(&prog, 0, nullptr), 0);
    set_shade(&prog, 1);
    CHECK(be::host_of<be::Program>(&prog) == nullptr);
    CHECK_EQ(static_cast<unsigned>(static_cast<unsigned char>(prog.reserved[0])), 0xABu);   // untouched
    CHECK_EQ(set_shaders(nullptr, 0, nullptr), 0);
}

NXTEST(subroutine_linkage_and_sample_shading_are_accepted) {
    setup();
    auto init = abi::proc<PFNNVNPROGRAMINITIALIZEPROC>("nvnProgramInitialize");
    auto fin = abi::proc<PFNNVNPROGRAMFINALIZEPROC>("nvnProgramFinalize");
    auto set_sub = abi::proc<PFNNVNPROGRAMSETSUBROUTINELINKAGEPROC>("nvnProgramSetSubroutineLinkage");
    auto set_shade = abi::proc<PFNSAMPLESHADINGASSUMED>("nvnProgramSetSampleShading");
    NVNprogram prog;
    std::memset(&prog, 0, sizeof prog);
    init(&prog, nullptr);
    CHECK_EQ(set_sub(&prog, 1, nullptr), 1);   // accepted, ignored
    set_shade(&prog, 1);                        // accepted, ignored
    set_shade(&prog, 0);
    CHECK(be::host_of<be::Program>(&prog) != nullptr);
    fin(&prog);
}

NXTEST(copies_of_the_guest_object_share_the_host_object) {
    setup();
    auto init = abi::proc<PFNNVNPROGRAMINITIALIZEPROC>("nvnProgramInitialize");
    NVNprogram a, b;
    std::memset(&a, 0, sizeof a);
    init(&a, nullptr);
    std::memcpy(&b, &a, sizeof a);   // the game copies objects around; the header still names the same host object
    CHECK(be::host_of<be::Program>(&b) == be::host_of<be::Program>(&a));
    CHECK(be::host_of<be::Program>(&a) != nullptr);
}
