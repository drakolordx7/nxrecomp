// SPDX-License-Identifier: GPL-3.0-or-later
// The input source the hid service reads (include/nxrt/input_source.h).
#include <mutex>

#include "nxrt/input_source.h"

namespace nxrt::hid {

namespace {
std::mutex g_mu;
std::shared_ptr<InputSource> g_src;
}  // namespace

void set_input_source(std::shared_ptr<InputSource> src) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_src = std::move(src);
}

std::shared_ptr<InputSource> input_source() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_src;
}

}  // namespace nxrt::hid
