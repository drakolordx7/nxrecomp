// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Utils/SpanIOHelper.cs.
// The C# class is a set of static helpers that each take a `ref` cursor (Memory<byte> / Span<byte>) and move it past the bytes used.
// Here the cursor is an object: SpanIOHelper<uint8_t> wraps a writable Span<uint8_t>, SpanIOHelper<const uint8_t> a read-only one.
// Each call consumes bytes from the front, exactly like the C# ref-cursor overloads. The C# method names are kept.
#pragma once
#include <cstddef>
#include <cstring>
#include <type_traits>

#include "../audren_common.h"

namespace nxrt::audren {

template <class TByte>
class SpanIOHelper {
    static_assert(std::is_same_v<std::remove_const_t<TByte>, uint8_t>, "SpanIOHelper works on bytes");

public:
    explicit SpanIOHelper(Span<TByte> backingMemory) : _backingMemory(backingMemory) {}

    Span<TByte> Remaining() const { return _backingMemory; }

    template <class T>
    void Write(const T& data)
        requires(!std::is_const_v<TByte>)
    {
        static_assert(std::is_trivially_copyable_v<T>, "unmanaged");

        size_t size = sizeof(T);

        if (size > _backingMemory.Length) {
            NXAUDREN_FATAL("SpanIOHelper.Write: ArgumentOutOfRangeException (backingMemory too small)");
        }

        std::memcpy(static_cast<void*>(_backingMemory.Data), static_cast<const void*>(&data), size);

        _backingMemory = _backingMemory.Slice(size);
    }

    template <class T>
    Span<T> GetWriteRef()
        requires(!std::is_const_v<TByte>)
    {
        static_assert(std::is_trivially_copyable_v<T>, "unmanaged");

        size_t size = sizeof(T);

        if (size > _backingMemory.Length) {
            NXAUDREN_FATAL("SpanIOHelper.GetWriteRef: ArgumentOutOfRangeException (backingMemory too small)");
        }

        Span<T> result = _backingMemory.Slice(0, size).template Cast<T>();

        _backingMemory = _backingMemory.Slice(size);

        return result;
    }

    template <class T>
    T Read() {
        static_assert(std::is_trivially_copyable_v<T>, "unmanaged");

        size_t size = sizeof(T);

        if (size > _backingMemory.Length) {
            NXAUDREN_FATAL("SpanIOHelper.Read: ArgumentOutOfRangeException (not enough data)");
        }

        T result{};
        std::memcpy(static_cast<void*>(&result), static_cast<const void*>(_backingMemory.Data), size);

        _backingMemory = _backingMemory.Slice(size);

        return result;
    }

    template <class T>
    static Span<T> GetMemory(Span<T> memory, int32_t id, uint32_t count) {
        NXAUDREN_ASSERT(id >= 0 && static_cast<uint32_t>(id) < count);

        return memory.Slice(static_cast<size_t>(id), 1);
    }

    template <class T>
    static T& GetFromMemory(Span<T> memory, int32_t id, uint32_t count) {
        return GetMemory(memory, id, count)[0];
    }

private:
    Span<TByte> _backingMemory;
};

}
