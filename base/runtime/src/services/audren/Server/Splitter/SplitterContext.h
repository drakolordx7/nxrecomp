// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Splitter/SplitterContext.cs (Ryubing fork, MIT).
// Owns the splitter states and the splitter destinations (version 1 or version 2, chosen by the revision), and parses the splitter update
// from the user input buffer.
// Deviations: the C# Memory<T> members are Span<T> views into the work buffer. The C# SequenceReader<byte> is SequenceReader. The
// C# Version / IsBugFixed / ... auto-properties are public data members. The C# class-typed parameters are references.
#pragma once
#include <cstdint>

#include "../../Common/WorkBufferAllocator.h"
#include "../../Utils/BitUtils.h"
#include "../../Utils/SpanIOHelper.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../BehaviourInfo.h"
#include "ISplitterDestinationInParameter.h"
#include "SequenceReader.h"
#include "SplitterDestination.h"
#include "SplitterState.h"

namespace nxrt::audren {

class SplitterContext {
  public:
    static constexpr int32_t BqfStatesPerDestination = 4;

    // Version of the splitter context that is being used, currently can be 1 or 2.
    int32_t Version = 0;
    bool IsBugFixed = false;
    // If set to true, the previous mix volume is explicitly resetted using the input parameter, instead of implicitly on first use.
    bool IsSplitterPrevVolumeResetSupported = false;
    bool IsBiquadFilterParameterFloatSupported = false;

    bool Initialize(BehaviourInfo& behaviourInfo, AudioRendererConfiguration& parameter, WorkBufferAllocator& workBufferAllocator,
                    Span<BiquadFilterState> splitterBqfStates) {
        if (!behaviourInfo.IsSplitterSupported() || parameter.splitter_count <= 0 || parameter.splitter_destination_count <= 0) {
            Setup(Span<SplitterState>(), Span<SplitterDestinationVersion1>(), Span<SplitterDestinationVersion2>(), false);

            return true;
        }

        Span<SplitterState> splitters = workBufferAllocator.Allocate<SplitterState>(parameter.splitter_count, SplitterState::Alignment);

        if (splitters.IsEmpty()) {
            return false;
        }

        int32_t splitterId = 0;

        for (SplitterState& splitter : splitters) {
            splitter = SplitterState(splitterId++);
        }

        Span<SplitterDestinationVersion1> splitterDestinationsV1;
        Span<SplitterDestinationVersion2> splitterDestinationsV2;

        if (!behaviourInfo.IsBiquadFilterParameterForSplitterEnabled()) {
            Version = 1;

            splitterDestinationsV1 =
                workBufferAllocator.Allocate<SplitterDestinationVersion1>(parameter.splitter_destination_count, SplitterDestinationVersion1::Alignment);

            if (splitterDestinationsV1.IsEmpty()) {
                return false;
            }

            int32_t splitterDestinationId = 0;
            for (SplitterDestinationVersion1& data : splitterDestinationsV1) {
                data = SplitterDestinationVersion1(splitterDestinationId++);
            }
        } else {
            Version = 2;

            splitterDestinationsV2 =
                workBufferAllocator.Allocate<SplitterDestinationVersion2>(parameter.splitter_destination_count, SplitterDestinationVersion2::Alignment);

            if (splitterDestinationsV2.IsEmpty()) {
                return false;
            }

            int32_t splitterDestinationId = 0;
            for (SplitterDestinationVersion2& data : splitterDestinationsV2) {
                data = SplitterDestinationVersion2(splitterDestinationId++);
            }

            if (parameter.splitter_destination_count > 0) {
                // Official code stores it in the SplitterDestinationVersion2 struct, but this port keeps it here to avoid unsafe code.
                splitterBqfStates.Clear();
                _splitterBqfStates = splitterBqfStates;
            } else {
                _splitterBqfStates = Span<BiquadFilterState>();
            }
        }

        IsSplitterPrevVolumeResetSupported = behaviourInfo.IsSplitterPrevVolumeResetSupported();
        IsBiquadFilterParameterFloatSupported = behaviourInfo.IsBiquadFilterParameterFloatSupported();

        SplitterState::InitializeSplitters(splitters);

        Setup(splitters, splitterDestinationsV1, splitterDestinationsV2, behaviourInfo.IsSplitterBugFixed());

        return true;
    }

    static uint64_t GetWorkBufferSize(uint64_t size, BehaviourInfo& behaviourInfo, AudioRendererConfiguration& parameter) {
        if (behaviourInfo.IsSplitterSupported()) {
            size = WorkBufferAllocator::GetTargetSize<SplitterState>(size, parameter.splitter_count, SplitterState::Alignment);

            if (behaviourInfo.IsBiquadFilterParameterForSplitterEnabled()) {
                size = WorkBufferAllocator::GetTargetSize<SplitterDestinationVersion2>(size, parameter.splitter_destination_count,
                                                                                      SplitterDestinationVersion2::Alignment);
            } else {
                size = WorkBufferAllocator::GetTargetSize<SplitterDestinationVersion1>(size, parameter.splitter_destination_count,
                                                                                      SplitterDestinationVersion1::Alignment);
            }

            if (behaviourInfo.IsSplitterBugFixed()) {
                size = WorkBufferAllocator::GetTargetSize<int32_t>(size, parameter.splitter_destination_count, 0x10);
            }
        }

        return size;
    }

    int32_t GetDestinationCountPerStateForCompatibility() const {
        if (_splitters.IsEmpty()) {
            return 0;
        }

        int32_t length = 0;

        if (_splitterDestinationsV2.IsEmpty()) {
            length = static_cast<int32_t>(_splitterDestinationsV1.Length);
        } else {
            length = static_cast<int32_t>(_splitterDestinationsV2.Length);
        }

        return length / static_cast<int32_t>(_splitters.Length);
    }

    bool Update(SequenceReader& input) {
        if (!UsingSplitter()) {
            return true;
        }

        const SplitterInParameterHeader header = input.GetRefOrRefToCopy<SplitterInParameterHeader>();

        if (header.IsMagicValid()) {
            ClearAllNewConnectionFlag();

            UpdateState(header, input);
            UpdateData(header, input);

            input.SetConsumed(BitUtils::AlignUp(input.Consumed(), static_cast<int64_t>(0x10)));

            return true;
        }

        input.Rewind(static_cast<int64_t>(sizeof(SplitterInParameterHeader)));

        return false;
    }

    SplitterState& GetState(int32_t id) {
        return SpanIOHelper<uint8_t>::GetFromMemory(_splitters, id, static_cast<uint32_t>(_splitters.Length));
    }

    SplitterDestination GetDestination(int32_t id) {
        if (_splitterDestinationsV2.IsEmpty()) {
            return SplitterDestination(SpanIOHelper<uint8_t>::GetFromMemory(_splitterDestinationsV1, id,
                                                                            static_cast<uint32_t>(_splitterDestinationsV1.Length)));
        }

        return SplitterDestination(SpanIOHelper<uint8_t>::GetFromMemory(_splitterDestinationsV2, id,
                                                                        static_cast<uint32_t>(_splitterDestinationsV2.Length)));
    }

    SplitterDestination GetDestination(int32_t id, int32_t destinationId) {
        SplitterState& splitter = GetState(id);

        return splitter.GetData(destinationId);
    }

    Span<BiquadFilterState> GetBiquadFilterState(SplitterDestination destination) {
        return _splitterBqfStates.Slice(static_cast<size_t>(destination.Id()) * BqfStatesPerDestination, BqfStatesPerDestination);
    }

    bool UsingSplitter() const {
        return !_splitters.IsEmpty() && (!_splitterDestinationsV1.IsEmpty() || !_splitterDestinationsV2.IsEmpty());
    }

    void UpdateInternalState() {
        for (SplitterState& splitter : _splitters) {
            splitter.UpdateInternalState();
        }
    }

  private:
    void Setup(Span<SplitterState> splitters, Span<SplitterDestinationVersion1> splitterDestinationsV1,
               Span<SplitterDestinationVersion2> splitterDestinationsV2, bool isBugFixed) {
        _splitters = splitters;
        _splitterDestinationsV1 = splitterDestinationsV1;
        _splitterDestinationsV2 = splitterDestinationsV2;
        IsBugFixed = isBugFixed;
    }

    void ClearAllNewConnectionFlag() {
        for (SplitterState& splitter : _splitters) {
            splitter.ClearNewConnectionFlag();
        }
    }

    void UpdateState(const SplitterInParameterHeader& inputHeader, SequenceReader& input) {
        for (uint32_t i = 0; i < inputHeader.splitter_count; i++) {
            const SplitterInParameter parameter = input.GetRefOrRefToCopy<SplitterInParameter>();

            NXAUDREN_ASSERT(parameter.IsMagicValid());

            if (parameter.IsMagicValid()) {
                if (parameter.id >= 0 && parameter.id < static_cast<int32_t>(_splitters.Length)) {
                    SplitterState& splitter = GetState(parameter.id);

                    splitter.Update(*this, parameter, input);
                }

                // NOTE: there are 12 bytes of unused/unknown data after the destination IDs array.
                input.Advance(0xC);
            } else {
                input.Rewind(static_cast<int64_t>(sizeof(SplitterInParameter)));
                break;
            }
        }
    }

    template <class T>
    bool UpdateData(SequenceReader& input) {
        using P = ISplitterDestinationInParameter<T>;

        const T parameter = input.GetRefOrRefToCopy<T>();

        NXAUDREN_ASSERT(P::IsMagicValid(parameter));

        if (P::IsMagicValid(parameter)) {
            int32_t length = 0;

            if (_splitterDestinationsV2.IsEmpty()) {
                length = static_cast<int32_t>(_splitterDestinationsV1.Length);
            } else {
                length = static_cast<int32_t>(_splitterDestinationsV2.Length);
            }

            if (P::Id(parameter) >= 0 && P::Id(parameter) < length) {
                SplitterDestination destination = GetDestination(P::Id(parameter));

                destination.Update(parameter, IsSplitterPrevVolumeResetSupported);
            }

            return true;
        }

        input.Rewind(static_cast<int64_t>(sizeof(T)));

        return false;
    }

    void UpdateData(const SplitterInParameterHeader& inputHeader, SequenceReader& input) {
        for (uint32_t i = 0; i < inputHeader.splitter_destination_count; i++) {
            if (Version == 1) {
                if (!UpdateData<SplitterDestinationInParameterVersion1>(input)) {
                    break;
                }
            } else if (Version == 2) {
                if (IsBiquadFilterParameterFloatSupported) {
                    if (!UpdateData<SplitterDestinationInParameterVersion2b>(input)) {
                        break;
                    }
                } else {
                    if (!UpdateData<SplitterDestinationInParameterVersion2a>(input)) {
                        break;
                    }
                }
            } else {
                // C# Debug.Fail: nothing is updated in release builds.
                NXAUDREN_ASSERT(false && "Invalid splitter context version");
            }
        }
    }

    Span<SplitterState> _splitters;
    Span<SplitterDestinationVersion1> _splitterDestinationsV1;
    Span<SplitterDestinationVersion2> _splitterDestinationsV2;
    Span<BiquadFilterState> _splitterBqfStates;
};

// SplitterState::Update (declared in SplitterState.h). The C# body reads the destination ids, links the destinations and skips the
// unknown data that follows, as in SplitterState.cs.
inline void SplitterState::Update(SplitterContext& context, const SplitterInParameter& parameter, SequenceReader& input) {
    ClearLinks();

    int32_t destinationCount = 0;

    if (context.IsBugFixed) {
        destinationCount = parameter.destination_count;
    } else {
        destinationCount = std::min(context.GetDestinationCountPerStateForCompatibility(), parameter.destination_count);
    }

    if (destinationCount > 0) {
        int32_t destinationId = 0;
        input.ReadLittleEndian(destinationId);

        SplitterDestination destination = context.GetDestination(destinationId);

        SetDestination(destination);

        DestinationCount = destinationCount;

        for (int32_t i = 1; i < destinationCount; i++) {
            input.ReadLittleEndian(destinationId);

            SplitterDestination nextDestination = context.GetDestination(destinationId);

            destination.Link(nextDestination);
            destination = nextDestination;
        }
    }

    if (destinationCount < parameter.destination_count) {
        input.Advance(static_cast<int64_t>(parameter.destination_count - destinationCount) * static_cast<int64_t>(sizeof(int32_t)));
    }

    NXAUDREN_ASSERT(parameter.id == Id);

    if (parameter.id == Id) {
        SampleRate = parameter.sample_rate;
        HasNewConnection = true;
    }
}

}
