// SPDX-License-Identifier: GPL-3.0-or-later
// Stub services: every command is logged and answered with kernel::kStubService (see include/nxrt/ipc.h).
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"

namespace nxrt::ipc {

namespace detail {
uint64_t note_unhandled(Service& s, uint32_t cmd);   // ipc.cpp: counts the command, returns the per-(service, cmd) count
}  // namespace detail

uint32_t StubService::handle(Request& req, Response&) {
    const uint64_t n = detail::note_unhandled(*this, req.cmd);
    // The first 20 calls per (service, command) are logged, then every 1000th.
    if (n <= 20 || n % 1000 == 0) {
        NXLOG_W("ipc",
                "STUB SERVICE %s cmd %u (in %zu bytes, %zu copy, %zu move handles, buffers a%zu b%zu x%zu c%zu w%zu) -> 0x%x",
                name().c_str(), req.cmd, req.in_size, req.in_copy.size(), req.in_move.size(), req.a.size(), req.b.size(),
                req.x.size(), req.c.size(), req.w.size(), kernel::kStubService);
    }
    return kernel::kStubService;
}

}  // namespace nxrt::ipc
