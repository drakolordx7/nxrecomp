// SPDX-License-Identifier: GPL-3.0-or-later
// Skeleton: replaced by the real acc:u0 (S3c). The accapi function below is a placeholder.
#include "nxrt/services_api.h"
#include "services_internal.h"

namespace nxrt::services {

void register_acc() {}
void acc_reset() {}

namespace accapi {
void user_uid(uint64_t* hi, uint64_t* lo) {
    const TitleProfile& p = profile();
    if (hi) *hi = p.user_uid_hi;
    if (lo) *lo = p.user_uid_lo;
}
}  // namespace accapi

}  // namespace nxrt::services
