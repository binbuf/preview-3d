#pragma once

#include "WireFormat.h"
#include "VertexLayouts.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace model_core {

// Worker/fixture utility. The broker independently recomputes bounds from its
// private snapshot; calling this is never a substitute for host validation.
inline bool SetLocalBounds(ChunkDescriptor& descriptor, std::span<const std::byte> vertices)
{
    const auto stride = VertexStrideForLayout(static_cast<VertexLayoutId>(descriptor.vertexLayoutId));
    if (!stride || !descriptor.vertexCount || vertices.size() < uint64_t(descriptor.vertexCount) * stride)
        return false;
    for (uint32_t i = 0; i < descriptor.vertexCount; ++i) {
        float xyz[3];
        std::memcpy(xyz, vertices.data() + size_t(i) * stride, sizeof(xyz));
        for (unsigned axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(xyz[axis])) return false;
            if (!i) descriptor.localMin[axis] = descriptor.localMax[axis] = xyz[axis];
            else {
                descriptor.localMin[axis] = (std::min)(descriptor.localMin[axis], xyz[axis]);
                descriptor.localMax[axis] = (std::max)(descriptor.localMax[axis], xyz[axis]);
            }
        }
    }
    descriptor.boundsState = BoundsState::Verified;
    return true;
}

// Rebase an existing local float payload with an exact shift. Double-source
// adapters choose their initial origin before narrowing; this optional shift
// preserves residuals that cannot be absorbed into the double origin exactly.
// Existing bounded adapter storage is reused, never copied for the UI.
inline bool RebasePositions(ChunkDescriptor& descriptor, std::span<std::byte> vertices)
{
    const auto stride = VertexStrideForLayout(static_cast<VertexLayoutId>(descriptor.vertexLayoutId));
    if (!stride || !descriptor.vertexCount || vertices.size() < uint64_t(descriptor.vertexCount) * stride)
        return false;
    float first[3]; std::memcpy(first, vertices.data(), sizeof(first));
    double shift[3]{};
    for (unsigned axis = 0; axis < 3; ++axis) {
        const double candidate = descriptor.origin[axis] + double(first[axis]);
        if (candidate - descriptor.origin[axis] == double(first[axis])) {
            shift[axis] = double(first[axis]); descriptor.origin[axis] = candidate;
        }
    }
    for (uint32_t i = 0; i < descriptor.vertexCount; ++i) {
        float xyz[3]; auto* target = vertices.data() + size_t(i) * stride;
        std::memcpy(xyz, target, sizeof(xyz));
        for (unsigned axis = 0; axis < 3; ++axis) xyz[axis] = float(double(xyz[axis]) - shift[axis]);
        std::memcpy(target, xyz, sizeof(xyz));
    }
    return SetLocalBounds(descriptor, vertices);
}
} // namespace model_core
