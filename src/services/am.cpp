// SPDX-License-Identifier: GPL-3.0-or-later
// Skeleton: replaced by the real appletOE (stream S3c). The amctl functions below are placeholders.
#include "nxrt/services_api.h"
#include "services_internal.h"

namespace nxrt::services {

void register_am() {}
void am_reset() {}

namespace amctl {
void set_docked(bool) {}
void set_focus(bool) {}
bool docked() { return true; }
bool in_focus() { return true; }
}  // namespace amctl

}  // namespace nxrt::services
