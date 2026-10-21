// SPDX-License-Identifier: GPL-3.0-or-later
// Memory guard: a background thread polls the process's private commit (PrivateUsage); past the limit it calls the
// on_exceed callback (default: log the overrun, flush, TerminateProcess(3)). The footprint is logged every 30 s.
// Config: [memory] limit_gb (default 8) / env NXRT_MEMORY_LIMIT_GB; [memory] poll_ms (250).
#pragma once
#include <cstdint>

namespace nxrt {

struct MemGuardOptions {
    uint64_t limit_bytes = 0;   // 0: read [memory] limit_gb from the config (default 8 GB)
    uint32_t poll_ms = 0;       // 0: [memory] poll_ms (default 250)
    // Called once from the guard thread when the footprint exceeds the limit (it must not call memguard_stop: that joins this thread). Null = default (terminate the process).
    void (*on_exceed)(uint64_t footprint, uint64_t limit, void* user) = nullptr;
    void* user = nullptr;
};

uint64_t memguard_footprint();       // bytes of private commit of this process (0 if the query fails)
double memguard_limit_gb(const char* env_text);   // positive number or 8.0
void memguard_start(const MemGuardOptions& opt = {});
void memguard_stop();                // joins the guard thread
bool memguard_running();
uint64_t memguard_peak();            // highest footprint seen by the guard
bool memguard_tripped();             // on_exceed has fired

}  // namespace nxrt
