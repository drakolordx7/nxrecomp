// SPDX-License-Identifier: GPL-3.0-or-later
// Registration of the NVN implementations (S4d). Each family of entry points lives in its own api_*.cpp and exposes
// one register function, which calls NVN_REG (nvn_abi.h) for every function it implements.
#pragma once
#include "nvn_abi.h"
#include "nvn_decoded.h"

namespace nxrt::nvn {

void register_api_builders();
void register_api_sampler();
void register_api_texture_builder();
void register_api_blend_color();
void register_api_vertex();
void register_api_depth_poly_ms();

// Command buffer: lifecycle and memory bookkeeping (api_cmdbuf.cpp), recorders of the state and bind commands (api_cmd_state.cpp)
// and of draws, clears, copies, compute, counters, sync (api_cmd_work.cpp). All of them only record; no GPU involved.
void register_api_cmdbuf();
void register_api_cmd_state();
void register_api_cmd_work();

void register_api_device();
void register_api_memory();
void register_api_texture();
void register_api_program();

// Registers every implementation above; called by install_backend(). Idempotent.
void register_all_impls();

}  // namespace nxrt::nvn
