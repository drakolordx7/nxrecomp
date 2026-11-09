// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Common/EdgeMatrix.cs. An adjacency matrix stored as bits (used for splitter routing).
#pragma once
#include "../audren_common.h"
#include "../Constants.h"
#include "../Utils/BitArray.h"
#include "../Utils/BitUtils.h"

namespace nxrt::audren {

class EdgeMatrix {
public:
    static int32_t GetWorkBufferSize(int32_t nodeCount) {
        int32_t size = BitUtils::AlignUp(nodeCount * nodeCount, Constants::BufferAlignment);
        return size / static_cast<int32_t>(sizeof(uint8_t));
    }

    void Initialize(Span<uint8_t> edgeMatrixWorkBuffer, int32_t nodeCount) {
        NXAUDREN_ASSERT(static_cast<int64_t>(edgeMatrixWorkBuffer.Length) >= GetWorkBufferSize(nodeCount));

        _storage = BitArray(edgeMatrixWorkBuffer);

        _nodeCount = nodeCount;

        _storage.Reset();
    }

    bool Test(int32_t index) const { return _storage.Test(index); }

    void Reset() { _storage.Reset(); }

    void Reset(int32_t index) { _storage.Reset(index); }

    void Set(int32_t index) { _storage.Set(index); }

    void Connect(int32_t source, int32_t destination) {
        NXAUDREN_ASSERT(source < _nodeCount);
        NXAUDREN_ASSERT(destination < _nodeCount);

        _storage.Set(_nodeCount * source + destination);
    }

    bool Connected(int32_t source, int32_t destination) const {
        NXAUDREN_ASSERT(source < _nodeCount);
        NXAUDREN_ASSERT(destination < _nodeCount);

        return _storage.Test(_nodeCount * source + destination);
    }

    void Disconnect(int32_t source, int32_t destination) {
        NXAUDREN_ASSERT(source < _nodeCount);
        NXAUDREN_ASSERT(destination < _nodeCount);

        _storage.Reset(_nodeCount * source + destination);
    }

    void RemoveEdges(int32_t source) {
        for (int32_t i = 0; i < _nodeCount; i++) {
            Disconnect(source, i);
        }
    }

    int32_t GetNodeCount() const { return _nodeCount; }

private:
    BitArray _storage;

    int32_t _nodeCount = 0;
};

}
