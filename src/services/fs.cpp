// SPDX-License-Identifier: GPL-3.0-or-later
// Skeleton: replaced by the real fsp-srv (S3c). The fsapi functions below are placeholders other services link against.
#include "nxrt/services_api.h"
#include "services_internal.h"

namespace nxrt::services {

void register_fs() {}
void fs_reset() {}

namespace fsapi {
uint32_t ensure_account_save(uint64_t, uint64_t, bool* created) {
    if (created) *created = false;
    return 0;
}
uint32_t account_save_size(uint64_t, uint64_t, int64_t* size, int64_t* journal) {
    if (size) *size = 4 << 20;
    if (journal) *journal = 1 << 20;
    return 0;
}
}  // namespace fsapi

}  // namespace nxrt::services
