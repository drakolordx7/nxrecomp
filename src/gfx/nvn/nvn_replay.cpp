// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER: replaced by the command list interpreter.
#include "nvn_gpu_internal.h"
namespace nxrt::nvn::be::gpu {
void replay_begin_submit(Frame&) {}
void replay_lists(Frame&, const std::vector<Ref<CmdList>>&) {}
void replay_end_submit(Frame&) {}
void replay_shutdown() {}
void present_on_render_thread(Window*, int) {}
}  // namespace nxrt::nvn::be::gpu
