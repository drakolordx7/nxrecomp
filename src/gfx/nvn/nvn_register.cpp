// SPDX-License-Identifier: GPL-3.0-or-later
#include "nvn_impl.h"

#include <mutex>

namespace nxrt::nvn {

void register_all_impls() {
    static std::once_flag once;
    std::call_once(once, [] {
        register_api_builders();
        register_api_sampler();
        register_api_texture_builder();
        register_api_blend_color();
        register_api_vertex();
        register_api_depth_poly_ms();
        register_api_gpu();
    });
}

}  // namespace nxrt::nvn
