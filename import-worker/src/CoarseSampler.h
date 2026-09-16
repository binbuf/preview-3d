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
#include <ppl.h>

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
    std::array<double,3> inverseSpan{};
    for (unsigned axis=0;axis<3;++axis) {
        const double span=double(d.localMax[axis])-d.localMin[axis];
        inverseSpan[axis]=span>0 ? 4.0/span : 0.0;
    }
    auto vertexIndex = [&](uint32_t primitive, unsigned corner) {
        if (!points && (d.geometryFlags & kGeometryDeindexed)) return primitive*3+corner;
        uint32_t index = primitive;
        if (!points) std::memcpy(&index, indices.data() + (size_t(primitive)*3+corner)*4, 4);
        return index;
    };
    const bool parallelDeindexed=!points && (d.geometryFlags&kGeometryDeindexed) && count>=16384;
    if (parallelDeindexed) {
        constexpr uint32_t kBlockPrimitives=8192;
        struct LocalCandidates {
            std::array<Candidate,64> candidates{};
            std::array<uint32_t,6> extrema{};
            std::array<double,6> values{};
        };
        const uint32_t blocks=(count+kBlockPrimitives-1)/kBlockPrimitives;
        std::vector<LocalCandidates> local(blocks);
        concurrency::parallel_for(uint32_t(0),blocks,[&](uint32_t block) {
            auto& output=local[block];
            for (auto& candidate:output.candidates) candidate.key=UINT64_MAX;
            const uint32_t begin=block*kBlockPrimitives,end=(std::min)(count,begin+kBlockPrimitives);
            for (uint32_t primitive=begin;primitive<end;++primitive) {
                double center[3]{}; uint64_t key=14695981039346656037ull;
                for (unsigned corner=0;corner<3;++corner) {
                    float position[3]; std::memcpy(position,vertices.data()+size_t(primitive*3+corner)*stride,sizeof(position));
                    uint32_t bits[3];std::memcpy(bits,position,sizeof(bits));
                    key^=uint64_t(bits[0])|(uint64_t(bits[1])<<32);key*=1099511628211ull;
                    key^=bits[2];key*=1099511628211ull;
                    for (unsigned axis=0;axis<3;++axis) {
                        center[axis]+=position[axis]/3.0;
                        for (unsigned side=0;side<2;++side) {
                            const unsigned slot=axis*2+side;
                            if ((primitive==begin && !corner) || (side ? position[axis]>output.values[slot]
                                : position[axis]<output.values[slot])) {
                                output.values[slot]=position[axis];output.extrema[slot]=primitive;
                            }
                        }
                    }
                }
                uint32_t cell=0;
                for (unsigned axis=0;axis<3;++axis) {
                    const uint32_t coordinate=inverseSpan[axis]>0 ? uint32_t(std::clamp(
                        (center[axis]-d.localMin[axis])*inverseSpan[axis],0.0,3.0)) : 0;
                    cell|=coordinate<<(axis*2);
                }
                auto& candidate=output.candidates[cell];
                if (key<candidate.key || (key==candidate.key && primitive<candidate.source))
                    candidate={cell,primitive,key};
            }
        });
        for (uint32_t block=0;block<blocks;++block) {
            for (size_t cell=0;cell<candidates.size();++cell) {
                const auto& candidate=local[block].candidates[cell];
                auto& combined=candidates[cell];
                if (candidate.key<combined.key || (candidate.key==combined.key && candidate.source<combined.source))
                    combined=candidate;
            }
            for (unsigned slot=0;slot<6;++slot) {
                const bool better=!block || (slot&1 ? local[block].values[slot]>extremeValues[slot]
                    : local[block].values[slot]<extremeValues[slot]);
                if (better) { extremeValues[slot]=local[block].values[slot];extrema[slot]=local[block].extrema[slot]; }
            }
        }
        if (cancelled && cancelled()) return {};
    }
    if (!parallelDeindexed) for (uint32_t primitive=0; primitive<count; ++primitive) {
        if (primitive%65536==0 && cancelled && cancelled()) return {};
        double center[3]{};
        uint64_t key = 14695981039346656037ull;
        for (unsigned corner=0; corner<(points ? 1u : 3u); ++corner) {
            auto vertex = vertices.subspan(size_t(vertexIndex(primitive,corner))*stride,stride);
            float position[3]; std::memcpy(position, vertex.data(), sizeof(position));
            // Representative selection depends on geometry, not on normals,
            // colors, UVs, or padding. Mix the three position bit patterns
            // directly instead of checksumming the entire vertex for every
            // primitive in a multi-GiB scan.
            uint32_t bits[3]; std::memcpy(bits,position,sizeof(bits));
            key^=uint64_t(bits[0])|(uint64_t(bits[1])<<32); key*=1099511628211ull;
            key^=bits[2]; key*=1099511628211ull;
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
            const uint32_t coordinate=inverseSpan[axis]>0 ? uint32_t(std::clamp(
                (center[axis]-d.localMin[axis])*inverseSpan[axis],0.0,3.0)) : 0;
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
            const uint32_t coordinate=inverseSpan[axis]>0 ? uint32_t(std::clamp(
                (center[axis]-d.localMin[axis])*inverseSpan[axis],0.0,3.0)) : 0;
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
