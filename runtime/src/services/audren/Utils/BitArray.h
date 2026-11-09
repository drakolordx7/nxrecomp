// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Utils/BitArray.cs. A bit array over a non-owning byte span.
#pragma once
#include "../audren_common.h"

namespace nxrt::audren {

class BitArray {
public:
    // C# has no parameterless constructor; EdgeMatrix holds a BitArray as a member that is bound later by Initialize.
    BitArray() = default;

    explicit BitArray(Span<uint8_t> storage) : _storage(storage) {}

    bool Test(int32_t index) const {
        uint8_t mask = static_cast<uint8_t>(1u << ToBitPosition(index));
        return (_storage[ToPosition(index)] & mask) == mask;
    }

    void Set(int32_t index) { Set(index, true); }

    void Reset(int32_t index) { Set(index, false); }

    void Reset() { _storage.Clear(); }

private:
    static int32_t ToPosition(int32_t index) { return index / 8; }

    static int32_t ToBitPosition(int32_t index) { return index % 8; }

    void Set(int32_t index, bool value) {
        uint8_t mask = static_cast<uint8_t>(1u << ToBitPosition(index));
        if (value) {
            _storage[ToPosition(index)] |= mask;
        } else {
            _storage[ToPosition(index)] &= static_cast<uint8_t>(~mask);
        }
    }

    Span<uint8_t> _storage;
};

}
