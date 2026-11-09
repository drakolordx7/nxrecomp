// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Voice/VoiceContext.cs (Ryubing fork, MIT).
//
// C# Memory<T> storage becomes Span<T> (non-owning). The sort is the C# Span.Sort comparison: priority first, then sorting order.
// Like C# (introsort), the order of two voices with the same priority AND sorting order is unspecified.
#pragma once
#include <algorithm>
#include <cstdint>

#include "../../Constants.h"
#include "../../Utils/SpanIOHelper.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "VoiceChannelResource.h"
#include "VoiceInfo.h"

namespace nxrt::audren {

class VoiceContext {
  private:
    Span<int32_t> _sortedVoices;

    Span<VoiceInfo> _voices;

    Span<VoiceChannelResource> _voiceChannelResources;

    Span<VoiceState> _voiceStatesCpu;

    Span<VoiceState> _voiceStatesDsp;

    uint32_t _voiceCount = 0;

  public:
    void Initialize(Span<int32_t> sortedVoices, Span<VoiceInfo> voices, Span<VoiceChannelResource> voiceChannelResources,
                    Span<VoiceState> voiceStatesCpu, Span<VoiceState> voiceStatesDsp, uint32_t voiceCount) {
        _sortedVoices = sortedVoices;
        _voices = voices;
        _voiceChannelResources = voiceChannelResources;
        _voiceStatesCpu = voiceStatesCpu;
        _voiceStatesDsp = voiceStatesDsp;
        _voiceCount = voiceCount;
    }

    uint32_t GetCount() const { return _voiceCount; }

    VoiceChannelResource& GetChannelResource(int32_t id) { return SpanIOHelper<uint8_t>::GetFromMemory(_voiceChannelResources, id, _voiceCount); }

    // Get the VoiceState span at the given id. Only for use when updating the server state.
    Span<VoiceState> GetUpdateStateForCpu(int32_t id) const { return SpanIOHelper<uint8_t>::GetMemory(_voiceStatesCpu, id, _voiceCount); }

    // Get the VoiceState span at the given id. Only for use in the context of processing on the AudioProcessor.
    Span<VoiceState> GetUpdateStateForDsp(int32_t id) const { return SpanIOHelper<uint8_t>::GetMemory(_voiceStatesDsp, id, _voiceCount); }

    VoiceInfo& GetState(int32_t id) { return SpanIOHelper<uint8_t>::GetFromMemory(_voices, id, _voiceCount); }

    VoiceInfo& GetSortedState(int32_t id) {
        NXAUDREN_ASSERT(id >= 0 && static_cast<uint32_t>(id) < _voiceCount);

        return GetState(_sortedVoices[static_cast<size_t>(id)]);
    }

    void UpdateForCommandGeneration() { _voiceStatesDsp.CopyTo(_voiceStatesCpu); }

    void Sort() {
        for (uint32_t i = 0; i < _voiceCount; i++) {
            _sortedVoices[i] = static_cast<int32_t>(i);
        }

        Span<int32_t> sortedVoicesTemp = _sortedVoices.Slice(0, _voiceCount);

        std::sort(sortedVoicesTemp.Data, sortedVoicesTemp.Data + sortedVoicesTemp.Length, [this](int32_t a, int32_t b) {
            const VoiceInfo& aInfo = GetState(a);
            const VoiceInfo& bInfo = GetState(b);

            if (aInfo.Priority != bInfo.Priority) {
                return aInfo.Priority < bInfo.Priority;
            }

            return aInfo.SortingOrder < bInfo.SortingOrder;
        });
    }
};

inline void VoiceInfo::ResetResources(VoiceContext& context) {
    for (uint32_t i = 0; i < ChannelsCount; i++) {
        int32_t channelResourceId = ChannelResourceIds[i];

        VoiceChannelResource& voiceChannelResource = context.GetChannelResource(channelResourceId);

        NXAUDREN_ASSERT(voiceChannelResource.IsUsed);

        Span<VoiceState> dspSharedState = context.GetUpdateStateForDsp(channelResourceId);

        dspSharedState.Cast<uint8_t>().Clear();

        voiceChannelResource.UpdateState();
    }
}

inline bool VoiceInfo::UpdateForCommandGeneration(VoiceContext& context) {
    if (IsNew) {
        ResetResources(context);
        PreviousVolume = Volume;
        IsNew = false;
    }

    // C# takes a Memory<VoiceState>[] from a pool; a local array of the same size is used instead.
    NXAUDREN_ASSERT(ChannelsCount <= VoiceChannelCountMax);
    std::array<Span<VoiceState>, VoiceChannelCountMax> voiceStates{};

    for (uint32_t i = 0; i < ChannelsCount; i++) {
        voiceStates[i] = context.GetUpdateStateForDsp(ChannelResourceIds[i]);
    }

    return UpdateParametersForCommandGeneration(Span<Span<VoiceState>>(voiceStates.data(), voiceStates.size()));
}

}
