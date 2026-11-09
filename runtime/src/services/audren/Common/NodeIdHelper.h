// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Common/NodeIdHelper.cs. Helper for manipulating node ids.
#pragma once
#include "../audren_common.h"
#include "../audren_types.h"

namespace nxrt::audren {

struct NodeIdHelper {
    static NodeIdType GetType(int32_t nodeId) { return static_cast<NodeIdType>(nodeId >> 28); }

    static int32_t GetBase(int32_t nodeId) { return (nodeId >> 16) & 0xFFF; }
};

}
