// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER (rule bytes all zero, no conversion): replaced by the real time zone code (stream S3c, svc-tz).
#include "timezone.h"

#include <cstring>

namespace nxrt::services::tz {

void make_utc_rule(uint8_t* out) { std::memset(out, 0, kRuleSize); }
bool make_host_rule(uint8_t* out, std::string* name_out) {
    make_utc_rule(out);
    if (name_out) *name_out = "UTC";
    return false;
}
bool to_calendar(const uint8_t*, int64_t, CalendarTime*, CalendarAdditionalInfo*) { return false; }
int to_posix(const uint8_t*, const CalendarTime&, int64_t*) { return 0; }

}  // namespace nxrt::services::tz
