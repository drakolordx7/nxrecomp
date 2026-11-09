// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Integration/IWritableEvent.cs (Ryubing fork, MIT): the system event an audio renderer session signals after
// each update. The C# interface is kept as an abstract class; the event itself is owned by the caller of the renderer.
#pragma once

namespace nxrt::audren {

class IWritableEvent {
  public:
    virtual ~IWritableEvent() = default;

    virtual void Signal() = 0;

    virtual void Clear() = 0;
};

}
