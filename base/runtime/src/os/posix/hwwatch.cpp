// SPDX-License-Identifier: GPL-3.0-or-later
// Linux implementation of nxrt/os/hwwatch.h: not provided (see the header). Callers poll the value instead.
#include "nxrt/os/hwwatch.h"

namespace nxrt::os {

bool hw_watch_supported() { return false; }
bool hw_watch_start(const uint64_t[2], HwWatchFn) { return false; }
void hw_watch_stop() {}

}  // namespace nxrt::os
