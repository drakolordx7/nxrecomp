// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Dsp/Effect/DecayDelay.cs (Ryubing fork, MIT). The wrapped delay line is held by reference,
// as the C# holds it by reference. Properties with a body (CurrentSampleCount, SampleCountMax) are methods.
#pragma once
#include "IDelayLine.h"

namespace nxrt::audren {

class DecayDelay : public IDelayLine {
  private:
    IDelayLine& _delayLine;

    float _decayRate = 0.0f;

  public:
    explicit DecayDelay(IDelayLine& delayLine) : _delayLine(delayLine) {}

    uint32_t CurrentSampleCount() const override { return _delayLine.CurrentSampleCount(); }

    uint32_t SampleCountMax() const override { return _delayLine.SampleCountMax(); }

    void SetDecayRate(float decayRate) { _decayRate = decayRate; }

    float Update(float value) override {
        float delayLineValue = _delayLine.Read();
        float processedValue = value - (_decayRate * delayLineValue);

        return _delayLine.Update(processedValue) + processedValue * _decayRate;
    }

    void SetDelay(float delayTime) override { _delayLine.SetDelay(delayTime); }

    float Read() override { return _delayLine.Read(); }

    float TapUnsafe(uint32_t sampleIndex, int32_t offset) override { return _delayLine.TapUnsafe(sampleIndex, offset); }

    float Tap(uint32_t sampleIndex) override { return _delayLine.Tap(sampleIndex); }
};

}  // namespace nxrt::audren
