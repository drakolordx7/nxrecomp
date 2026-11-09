// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Splitter/SplitterDestination.cs (Ryubing fork, MIT).
// The C# ref struct wraps either a version 1 or a version 2 destination (picked by the revision) through two references, one of which is
// null. Here the two references are two pointers, and IsNull() is the C# IsNull property.
// Deviations: GetV1RefOrNull/GetV2RefOrNull return pointers (null for the C# null ref). Members that are C# properties with a body are
// methods; the same applies to IsNull.
#pragma once
#include <cstdint>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "SplitterDestinationVersion1.h"
#include "SplitterDestinationVersion2.h"

namespace nxrt::audren {

class SplitterDestination {
  public:
    SplitterDestination() = default;

    explicit SplitterDestination(SplitterDestinationVersion1& v1) : _v1(&v1), _v2(nullptr) {}

    explicit SplitterDestination(SplitterDestinationVersion2& v2) : _v1(nullptr), _v2(&v2) {}

    SplitterDestination(SplitterDestinationVersion1* v1, SplitterDestinationVersion2* v2) : _v1(v1), _v2(v2) {}

    bool IsNull() const { return _v1 == nullptr && _v2 == nullptr; }

    int32_t Id() const {
        if (_v2 == nullptr) {
            if (_v1 == nullptr) {
                return 0;
            }

            return _v1->Id;
        }

        return _v2->Id;
    }

    int32_t DestinationId() const {
        if (_v2 == nullptr) {
            if (_v1 == nullptr) {
                return 0;
            }

            return _v1->DestinationId;
        }

        return _v2->DestinationId;
    }

    Span<float> MixBufferVolume() const {
        if (_v2 == nullptr) {
            if (_v1 == nullptr) {
                return Span<float>();
            }

            return _v1->MixBufferVolume();
        }

        return _v2->MixBufferVolume();
    }

    Span<float> PreviousMixBufferVolume() const {
        if (_v2 == nullptr) {
            if (_v1 == nullptr) {
                return Span<float>();
            }

            return _v1->PreviousMixBufferVolume();
        }

        return _v2->PreviousMixBufferVolume();
    }

    SplitterDestination Next() const {
        if (_v2 == nullptr) {
            if (_v1 == nullptr) {
                return SplitterDestination();
            }

            return SplitterDestination(_v1->Next(), nullptr);
        }

        return SplitterDestination(nullptr, _v2->Next());
    }

    template <class T>
    void Update(const T& parameter, bool isPrevVolumeResetSupported) {
        if (_v2 == nullptr) {
            _v1->Update(parameter, isPrevVolumeResetSupported);
        } else {
            _v2->Update(parameter, isPrevVolumeResetSupported);
        }
    }

    void UpdateInternalState() const {
        if (_v2 == nullptr) {
            _v1->UpdateInternalState();
        } else {
            _v2->UpdateInternalState();
        }
    }

    void MarkAsNeedToUpdateInternalState() const {
        if (_v2 == nullptr) {
            _v1->MarkAsNeedToUpdateInternalState();
        } else {
            _v2->MarkAsNeedToUpdateInternalState();
        }
    }

    bool IsConfigured() const {
        if (_v2 == nullptr) {
            return _v1->IsConfigured();
        }

        return _v2->IsConfigured();
    }

    float GetMixVolume(int32_t destinationIndex) const {
        if (_v2 == nullptr) {
            return _v1->GetMixVolume(destinationIndex);
        }

        return _v2->GetMixVolume(destinationIndex);
    }

    float GetMixVolumePrev(int32_t destinationIndex) const {
        if (_v2 == nullptr) {
            return _v1->GetMixVolumePrev(destinationIndex);
        }

        return _v2->GetMixVolumePrev(destinationIndex);
    }

    void ClearVolumes() const {
        if (_v2 == nullptr) {
            _v1->ClearVolumes();
        } else {
            _v2->ClearVolumes();
        }
    }

    void Link(SplitterDestination next) const {
        if (_v2 == nullptr) {
            NXAUDREN_ASSERT(next._v1 != nullptr);

            _v1->Link(*next._v1);
        } else {
            NXAUDREN_ASSERT(next._v2 != nullptr);

            _v2->Link(*next._v2);
        }
    }

    void Unlink() const {
        if (_v2 == nullptr) {
            _v1->Unlink();
        } else {
            _v2->Unlink();
        }
    }

    bool IsBiquadFilterEnabled() const { return _v2 != nullptr && _v2->IsBiquadFilterEnabled(); }

    bool IsBiquadFilterEnabledPrev() const { return _v2 != nullptr && _v2->IsBiquadFilterEnabledPrev(); }

    // Gets the biquad filter parameters. index: biquad filter index (0 or 1). Only valid for version 2 destinations.
    BiquadFilterParameter2& GetBiquadFilterParameter(int32_t index) const {
        NXAUDREN_ASSERT(_v2 != nullptr);

        return _v2->GetBiquadFilterParameter(index);
    }

    // Remember whether the biquad filter at index was enabled. Does nothing for version 1 destinations.
    void UpdateBiquadFilterEnabledPrev(int32_t index) const {
        if (_v2 != nullptr) {
            _v2->UpdateBiquadFilterEnabledPrev(index);
        }
    }

    SplitterDestinationVersion1* GetV1RefOrNull() const { return _v1; }

    SplitterDestinationVersion2* GetV2RefOrNull() const { return _v2; }

  private:
    SplitterDestinationVersion1* _v1 = nullptr;
    SplitterDestinationVersion2* _v2 = nullptr;
};

}
