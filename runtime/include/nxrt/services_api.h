// SPDX-License-Identifier: GPL-3.0-or-later
// Cross-service and host-facing entry points of the service library (stream S3c): what one service asks of another, and what the PC
// layers (window focus, settings UI) call to change console state. Implemented in the .cpp of the service named in each comment.
#pragma once
#include <cstdint>
#include <string>

namespace nxrt::services {

// ---- fs (fs.cpp): save data backend used by am (EnsureSaveData ...) and the fsp-srv commands ---------------------------------------
// A user id is the 128-bit nn::account::Uid as two u64 halves in memory order: the first u64 (uid[0]) is "hi", the second is "lo" (profile().user_uid_hi / _lo).
// Results are Horizon result codes (0 = success; the module-2 fs results otherwise).
namespace fsapi {
// Creates the account save data of the title for `uid` when it does not exist (size and journal from the profile). *created tells which.
uint32_t ensure_account_save(uint64_t uid_hi, uint64_t uid_lo, bool* created);
// Size and journal size of the account save data (the values nn::fs::GetSaveDataSize reports); the fs "path not found" result when absent.
uint32_t account_save_size(uint64_t uid_hi, uint64_t uid_lo, int64_t* size, int64_t* journal);
}  // namespace fsapi

// ---- am (am.cpp): console state changes from the host --------------------------------------------------------------------------------
// Each call queues the matching AppletMessage for the guest (it sees them through ICommonStateGetter::ReceiveMessage, with the message
// event signalled) and updates the state the getters report.
namespace amctl {
void set_docked(bool docked);          // OperationModeChanged (30) + PerformanceModeChanged (31); GetOperationMode/GetPerformanceMode follow
void set_focus(bool in_focus);         // FocusStateChanged (15); GetCurrentFocusState: 1 = InFocus, 2 = OutOfFocus
bool docked();
bool in_focus();
}  // namespace amctl

// ---- acc (acc.cpp) ----------------------------------------------------------------------------------------------------------------
namespace accapi {
void user_uid(uint64_t* hi, uint64_t* lo);   // the one local user (profile())
}

}  // namespace nxrt::services
