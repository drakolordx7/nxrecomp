// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Splitter/SplitterState.cs (Ryubing fork, MIT).
// Server state of one splitter: its sample rate and the head of a linked list of splitter destinations.
// Deviations: the C# Destination property is a method, and the head pointers are C++ pointers (C# unsafe pointers).
// Update() is declared here and defined at the end of SplitterContext.h, because it needs the full SplitterContext.
#pragma once
#include <algorithm>
#include <cstdint>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "SequenceReader.h"
#include "SplitterDestination.h"

namespace nxrt::audren {

class SplitterContext;

// C# StructLayout Size 0x20, Pack 0x10.
class alignas(0x10) SplitterState {
  public:
    static constexpr int32_t Alignment = 0x10;

    int32_t Id = 0;
    uint32_t SampleRate = 0;
    int32_t DestinationCount = 0;
    bool HasNewConnection = false;

    SplitterState() = default;

    explicit SplitterState(int32_t id) : Id(id) {}

    SplitterDestination Destination() const { return SplitterDestination(_destinationDataV1, _destinationDataV2); }

    SplitterDestination GetData(int32_t index) const {
        int32_t i = 0;

        SplitterDestination result = Destination();

        while (i < index) {
            if (result.IsNull()) {
                break;
            }

            result = result.Next();
            i++;
        }

        return result;
    }

    void ClearNewConnectionFlag() { HasNewConnection = false; }

    void Update(SplitterContext& context, const SplitterInParameter& parameter, SequenceReader& input);

    void SetDestination(SplitterDestination newValue) {
        _destinationDataV1 = newValue.GetV1RefOrNull();
        _destinationDataV2 = newValue.GetV2RefOrNull();
    }

    void UpdateInternalState() const {
        ForEachDestination([](SplitterDestination destination, int32_t) { destination.UpdateInternalState(); });
    }

    void ClearLinks() {
        ForEachDestination([](SplitterDestination destination, int32_t) { destination.Unlink(); });

        _destinationDataV1 = nullptr;
        _destinationDataV2 = nullptr;
    }

    static void InitializeSplitters(Span<SplitterState> splitters) {
        for (SplitterState& splitter : splitters) {
            splitter._destinationDataV1 = nullptr;
            splitter._destinationDataV2 = nullptr;

            splitter.DestinationCount = 0;
        }
    }

  private:
    // Utility function to apply an action to all Destination. The next element is read before the action, as in C#, because the action may
    // unlink the current element.
    template <class F>
    void ForEachDestination(F&& action) const {
        SplitterDestination temp = Destination();

        int32_t i = 0;

        while (true) {
            if (temp.IsNull()) {
                break;
            }

            SplitterDestination next = temp.Next();

            action(temp, i++);

            temp = next;
        }
    }

    SplitterDestinationVersion1* _destinationDataV1 = nullptr;
    SplitterDestinationVersion2* _destinationDataV2 = nullptr;
};

}
