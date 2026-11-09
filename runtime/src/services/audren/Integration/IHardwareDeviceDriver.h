// SPDX-License-Identifier: GPL-3.0-or-later
// Port of the output side of Ryujinx.Audio/Integration/IHardwareDeviceDriver.cs (Ryubing fork, MIT). Reduced to what the audio
// processor needs: opening one output device per session. The C# input direction, guest memory manager, volume and the update and
// pause events are not part of this port.
//
// Deviation: SupportsChannelCount is an extra non-pure virtual (C# IHardwareDeviceDriver.SupportsChannelCount, used by
// AudioProcessor.GetHardwareChannelCount). Its default answer is false, so an implementation that only opens stereo needs no override.
#pragma once
#include <cstdint>
#include <memory>

#include "IHardwareDevice.h"

namespace nxrt::audren {

class IHardwareDeviceDriver {
  public:
    virtual ~IHardwareDeviceDriver() = default;

    virtual std::unique_ptr<IHardwareDevice> OpenHardwareDevice(uint32_t sampleRate, uint32_t channelCount) = 0;

    virtual bool SupportsChannelCount(uint32_t channelCount) const {
        (void)channelCount;
        return false;
    }
};

}
