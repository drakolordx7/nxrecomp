// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Performance/IPerformanceHeader.cs (Ryubing fork, MIT).
// The C# interface becomes a C++20 concept (see IPerformanceDetailEntry.h for the reasoning).
#pragma once
#include <concepts>
#include <type_traits>

#include "../../audren_common.h"

namespace nxrt::audren {

template <class T>
concept IPerformanceHeader = std::is_trivially_copyable_v<T> && requires(T& t, const T& ct, bool flag, uint32_t u32, uint64_t u64, int32_t i32) {
    { ct.GetEntryCountOffset() } -> std::same_as<int32_t>;
    t.SetDspRunningBehind(flag);
    t.SetVoiceDropCount(u32);
    t.SetStartRenderingTicks(u64);
    t.SetMagic(u32);
    t.SetNextOffset(i32);
    t.SetTotalProcessingTime(i32);
    t.SetIndex(u32);
    { ct.GetEntryCount() } -> std::same_as<int32_t>;
    { ct.GetEntryDetailCount() } -> std::same_as<int32_t>;
    t.SetEntryCount(i32);
    t.SetEntryDetailCount(i32);
};

}
