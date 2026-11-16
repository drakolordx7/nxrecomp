// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to the platform layer: the handler chain shared by src/os/win32/fault.cpp and src/os/posix/fault.cpp (contract: nxrt/os/fault.h).
#pragma once
#include "nxrt/os/fault.h"

namespace nxrt::os::detail {

// Walks the chain; true when a handler returned Handled. Lock-free and allocation-free: safe inside a signal handler.
bool chain_run(const FaultInfo& fi);
// Calls the unhandled handler if one is set; true when it returned Handled.
bool unhandled_run(const FaultInfo& fi);

// Provided by each platform: install the process-wide dispatcher (called once, before the first handler becomes visible) ...
void platform_install();
// ... and told when the first unhandled handler is set (Windows hooks SetUnhandledExceptionFilter then, so a process without a reporter keeps the filter slot free).
void platform_unhandled_set();

}  // namespace nxrt::os::detail
