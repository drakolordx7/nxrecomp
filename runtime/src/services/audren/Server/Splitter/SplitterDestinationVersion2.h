// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Splitter/SplitterDestinationVersion2.cs (Ryubing fork, MIT).
// Server state of one splitter destination, used when the revision has biquad filters on splitters (revision 12 and later).
// Deviations: the C# Array2<T> fields are plain C arrays of 2 elements. Next() returns a pointer (null for no next element).
#pragma once
#include <cstddef>
#include <cstdint>

#include "../../Constants.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "ISplitterDestinationInParameter.h"

namespace nxrt::audren {

// C# StructLayout Size 0x128, Pack 0x10. Not alignas(16): 0x128 is not a multiple of 16. The reserved tail below makes sizeof 0x128, so the
// work buffer size (sizeof based) matches the C# GetWorkBufferSize.
class SplitterDestinationVersion2 {
  public:
    static constexpr int32_t Alignment = 0x10;

    int32_t Id = 0;
    int32_t DestinationId = 0;
    bool IsUsed = false;
    bool NeedToUpdateInternalState = false;

    SplitterDestinationVersion2() = default;

    explicit SplitterDestinationVersion2(int32_t id) : Id(id) {
        DestinationId = Constants::UnusedMixId;
        ClearVolumes();
    }

    Span<float> MixBufferVolume() { return Span<float>(_mix, kMixBufferCount); }
    Span<const float> MixBufferVolume() const { return Span<const float>(_mix, kMixBufferCount); }

    Span<float> PreviousMixBufferVolume() { return Span<float>(_previousMix, kMixBufferCount); }
    Span<const float> PreviousMixBufferVolume() const { return Span<const float>(_previousMix, kMixBufferCount); }

    SplitterDestinationVersion2* Next() const { return _next; }

    template <class T>
    void Update(const T& parameter, bool isPrevVolumeResetSupported) {
        using P = ISplitterDestinationInParameter<T>;

        NXAUDREN_ASSERT(Id == P::Id(parameter));

        if (P::IsMagicValid(parameter) && Id == P::Id(parameter)) {
            DestinationId = P::DestinationId(parameter);

            P::MixBufferVolume(parameter).CopyTo(MixBufferVolume());

            auto filters = P::BiquadFilters2(parameter);
            _biquadFilters[0] = filters[0];
            _biquadFilters[1] = filters[1];

            bool resetPrevVolume = isPrevVolumeResetSupported ? P::ResetPrevVolume(parameter) : (!IsUsed && P::IsUsed(parameter));
            if (resetPrevVolume) {
                MixBufferVolume().CopyTo(PreviousMixBufferVolume());

                NeedToUpdateInternalState = false;
            }

            IsUsed = P::IsUsed(parameter);
        }
    }

    void UpdateInternalState() {
        if (IsUsed && NeedToUpdateInternalState) {
            MixBufferVolume().CopyTo(PreviousMixBufferVolume());
        }

        NeedToUpdateInternalState = false;
    }

    void MarkAsNeedToUpdateInternalState() { NeedToUpdateInternalState = true; }

    bool IsConfigured() const { return IsUsed && DestinationId != Constants::UnusedMixId; }

    float GetMixVolume(int32_t destinationIndex) const {
        NXAUDREN_ASSERT(destinationIndex >= 0 && destinationIndex < Constants::MixBufferCountMax);

        return MixBufferVolume()[static_cast<size_t>(destinationIndex)];
    }

    float GetMixVolumePrev(int32_t destinationIndex) const {
        NXAUDREN_ASSERT(destinationIndex >= 0 && destinationIndex < Constants::MixBufferCountMax);

        return PreviousMixBufferVolume()[static_cast<size_t>(destinationIndex)];
    }

    void ClearVolumes() {
        MixBufferVolume().Clear();
        PreviousMixBufferVolume().Clear();
    }

    void Link(SplitterDestinationVersion2& next) { _next = &next; }

    void Unlink() { _next = nullptr; }

    bool IsBiquadFilterEnabled() const { return _biquadFilters[0].enable || _biquadFilters[1].enable; }

    bool IsBiquadFilterEnabledPrev() const { return _isPreviousBiquadFilterEnabled[0]; }

    BiquadFilterParameter2& GetBiquadFilterParameter(int32_t index) {
        NXAUDREN_ASSERT(index >= 0 && index < 2);

        return _biquadFilters[index];
    }

    void UpdateBiquadFilterEnabledPrev(int32_t index) {
        NXAUDREN_ASSERT(index >= 0 && index < 2);

        _isPreviousBiquadFilterEnabled[index] = _biquadFilters[index].enable;
    }

  private:
    static constexpr size_t kMixBufferCount = static_cast<size_t>(Constants::MixBufferCountMax);

    float _mix[kMixBufferCount] = {};
    float _previousMix[kMixBufferCount] = {};
    SplitterDestinationVersion2* _next = nullptr;
    BiquadFilterParameter2 _biquadFilters[2] = {};
    bool _isPreviousBiquadFilterEnabled[2] = {};
    // Unused tail of the C# struct (the C# Size is 0x128). The fields above end at offset 266 (public Id, DestinationId, IsUsed and
    // NeedToUpdateInternalState come first, then the 4-byte aligned volumes and the 8-byte next pointer).
    [[maybe_unused]] uint8_t _reservedToCSize[0x128 - 266] = {};
};

static_assert(sizeof(SplitterDestinationVersion2) == 0x128);

}
