#pragma once
#include "model_core/GeometryBounds.h"
#include "model_core/VertexLayouts.h"
#include "model_core/Checksum.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <span>
#include <functional>
#include <vector>

namespace import_worker {
inline std::vector<uint64_t> PreviewOffsets(uint64_t count,unsigned adjacent=1) {
    std::vector<uint64_t> offsets;
    for (uint64_t stratum=0;stratum<8;++stratum) for (unsigned point=0;point<adjacent;++point) {
        const uint64_t offset=stratum*count/8+point;
        if (offset<count) offsets.push_back(offset);
    }
    std::sort(offsets.begin(),offsets.end()); offsets.erase(std::unique(offsets.begin(),offsets.end()),offsets.end());
    return offsets;
}
struct CoarseSample {
    model_core::ChunkDescriptor descriptor{};
    std::vector<std::byte> vertices;
    std::vector<uint32_t> indices;
    uint64_t Primitives() const {
        return descriptor.topology == model_core::ChunkTopology::PointList
            ? descriptor.vertexCount : descriptor.indexCount / 3;
    }
};

// Bounded to a single normalized cluster. Spatial strata protect separated
// components; geometry hashes choose representatives independently of source
// order. Extrema protect boundaries, then source strata fill remaining density.
inline CoarseSample SampleCoarse(model_core::ChunkDescriptor d, std::span<const std::byte> vertices,
                                 std::span<const std::byte> indices, uint32_t quota,
                                 const std::function<bool()>& cancelled = {}) {
    using namespace model_core;
    const bool points = d.topology == ChunkTopology::PointList;
    const uint32_t count = points ? d.vertexCount : d.indexCount / 3;
    const size_t stride = VertexStrideForLayout(VertexLayoutId(d.vertexLayoutId));
    struct Candidate { uint32_t cell, source; uint64_t key; };
    std::array<Candidate,64> candidates{};
    for (auto& candidate:candidates) candidate.key=UINT64_MAX;
    std::array<uint32_t, 6> extrema{};
    std::array<double, 6> extremeValues{};
    auto vertexIndex = [&](uint32_t primitive, unsigned corner) {
        uint32_t index = primitive;
        if (!points) std::memcpy(&index, indices.data() + (size_t(primitive)*3+corner)*4, 4);
        return index;
    };
    for (uint32_t primitive=0; primitive<count; ++primitive) {
        if (primitive%4096==0 && cancelled && cancelled()) return {};
        double center[3]{};
        uint64_t key = 14695981039346656037ull;
        for (unsigned corner=0; corner<(points ? 1u : 3u); ++corner) {
            auto vertex = vertices.subspan(size_t(vertexIndex(primitive,corner))*stride,stride);
            key ^= Fnv1a64(vertex); key *= 1099511628211ull;
            float position[3]; std::memcpy(position, vertex.data(), sizeof(position));
            for (unsigned axis=0; axis<3; ++axis) {
                center[axis] += position[axis] / (points ? 1.0 : 3.0);
                for (unsigned side=0; side<2; ++side) {
                    const unsigned slot=axis*2+side;
                    if ((!primitive && !corner) || (side ? position[axis]>extremeValues[slot] : position[axis]<extremeValues[slot])) {
                        extremeValues[slot]=position[axis]; extrema[slot]=primitive;
                    }
                }
            }
        }
        uint32_t cell=0;
        for (unsigned axis=0; axis<3; ++axis) {
            const double span=double(d.localMax[axis])-d.localMin[axis];
            const uint32_t coordinate=span>0 ? uint32_t(std::clamp((center[axis]-d.localMin[axis])/span*4,0.0,3.0)) : 0;
            cell |= coordinate << (axis*2);
        }
        auto& candidate=candidates[cell];
        if (key<candidate.key || (key==candidate.key && primitive<candidate.source)) candidate={cell,primitive,key};
    }
    // Prefer boundary primitives within their occupied spatial cells without
    // letting multiple extrema consume the slots needed by other components.
    for (auto source:extrema) {
        double center[3]{};
        for (unsigned corner=0;corner<(points ? 1u : 3u);++corner) {
            float position[3]; std::memcpy(position,vertices.data()+size_t(vertexIndex(source,corner))*stride,12);
            for (unsigned axis=0;axis<3;++axis) center[axis]+=position[axis]/(points ? 1.0 : 3.0);
        }
        uint32_t cell=0;
        for (unsigned axis=0;axis<3;++axis) {
            const double span=double(d.localMax[axis])-d.localMin[axis];
            const uint32_t coordinate=span>0 ? uint32_t(std::clamp((center[axis]-d.localMin[axis])/span*4,0.0,3.0)) : 0;
            cell |= coordinate << (axis*2);
        }
        candidates[cell].source=source;
    }
    quota=(std::min)(quota,count);
    std::set<uint32_t> selected;
    auto add=[&](uint32_t source) { if (selected.size()<quota) selected.insert(source); };
    for (const auto& candidate:candidates) if (candidate.key!=UINT64_MAX) add(candidate.source);
    if (quota>=6) for (auto source:extrema) add(source);
    for (uint32_t stratum=0; stratum<quota; ++stratum)
        add(uint32_t((uint64_t(stratum)*count+count/2)/quota));
    for (uint32_t source=0;source<count && selected.size()<quota;++source) add(source);
    CoarseSample sample;
    sample.descriptor=d;
    auto& coarse=sample.descriptor;
    coarse.lodLevel=kCoarseLod;
    coarse.chunkId=kCoarseIdentity | d.chunkId;
    // Preserve all attributes and winding; deindexing avoids a source-sized remap.
    sample.vertices.reserve(selected.size()*(points ? 1 : 3)*stride);
    for (auto source:selected) for (unsigned corner=0; corner<(points ? 1u : 3u); ++corner) {
        auto vertex=vertices.subspan(size_t(vertexIndex(source,corner))*stride,stride);
        if (!points) sample.indices.push_back(uint32_t(sample.vertices.size()/stride));
        sample.vertices.insert(sample.vertices.end(),vertex.begin(),vertex.end());
    }
    coarse.vertexCount=uint32_t(sample.vertices.size()/stride);
    coarse.indexCount=uint32_t(sample.indices.size());
    SetLocalBounds(coarse,sample.vertices);
    return sample;
}
}
