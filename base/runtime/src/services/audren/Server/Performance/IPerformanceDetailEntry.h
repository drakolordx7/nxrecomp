// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Performance/IPerformanceDetailEntry.cs (Ryubing fork, MIT).
// The C# interface becomes a C++20 concept: the implementations (PerformanceDetailVersion1/2) are binary layouts written into the
// guest buffer, so they carry no vtable and the generic manager calls their methods statically.
#pragma once
#include <concepts>
#include <type_traits>

#include "../../audren_common.h"
#include "../../audren_types.h"

namespace nxrt::audren {

template <class T>
concept IPerformanceDetailEntry = std::is_trivially_copyable_v<T> && requires(T& t, const T& ct, int32_t value, PerformanceEntryType entryType,
                                                                             PerformanceDetailType detailType) {
    { ct.GetStartTime() } -> std::same_as<int32_t>;
    { ct.GetStartTimeOffset() } -> std::same_as<int32_t>;
    { ct.GetProcessingTime() } -> std::same_as<int32_t>;
    { ct.GetProcessingTimeOffset() } -> std::same_as<int32_t>;
    t.SetNodeId(value);
    t.SetEntryType(entryType);
    t.SetDetailType(detailType);
};

}
