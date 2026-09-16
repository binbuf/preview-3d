#pragma once
#include "ChunkBatchSink.h"
#include "CoarseSampler.h"
#include "model_core/Checksum.h"
#include "model_core/ImportError.h"
#include "model_core/MappedFile.h"
#include "model_core/TierALimits.h"
#include "model_core/WireFormat.h"
#include <algorithm>
#include <cstring>
#include <span>
#include <vector>
#include <deque>
#include <optional>

namespace import_worker
{
inline uint64_t TierAScratchLimit();
// Source payloads are copied into the reusable output window immediately.
// Only bounded coarse samples and fixed-width descriptors survive publication.
class BoundedMappedReader
{
  public:
    explicit BoundedMappedReader(model_core::MappedFile* file) : file_(file)
    {
    }
    std::optional<std::span<const std::byte>> Read(uint64_t offset, uint64_t count)
    {
        if (!file_ || offset > file_->SizeBytes() || count > file_->SizeBytes() - offset)
            return std::nullopt;
        if (!count)
            return std::span<const std::byte>{};
        for (auto& window : windows_)
            if (window.lease && offset >= window.start && count <= window.lease.Bytes().size() &&
                offset - window.start <= window.lease.Bytes().size() - count)
                return window.lease.Bytes().subspan(size_t(offset - window.start), size_t(count));
        auto& window = windows_[next_++ % 2];
        window.start = offset / (8ull * 1024 * 1024) * (8ull * 1024 * 1024);
        const uint64_t length = (std::min)(file_->SizeBytes() - window.start,
                                           (std::max)(8ull * 1024 * 1024, offset - window.start + count));
        std::wstring error;
        window.lease = file_->MapWindow(window.start, length, error);
        if (!window.lease)
            return std::nullopt;
        return window.lease.Bytes().subspan(size_t(offset - window.start), size_t(count));
    }

  private:
    struct Window
    {
        uint64_t start = 0;
        model_core::MappingLease lease;
    } windows_[2];
    model_core::MappedFile* file_;
    unsigned next_ = 0;
};
class BoundedChunkWriter
{
  public:
    BoundedChunkWriter(std::span<std::byte> output, uint64_t generation, uint32_t maxChunks,
                       model_core::SceneMetadata scene, ChunkBatchSink* sink)
        : output_(output), generation_(generation), maxChunks_(maxChunks), scene_(scene), sink_(sink)
    {
    }
    bool Add(model_core::ChunkDescriptor descriptor, std::span<const std::byte> a,
             std::span<const std::byte> b = {})
    {
        using namespace model_core;
        const bool geometry = descriptor.topology == ChunkTopology::TriangleList || descriptor.topology == ChunkTopology::PointList;
        std::optional<model_core::ScanSummaryPayload> scanSummary;
        if (sink_ && sink_->ProxyEnabled()) {
            if (sink_->Preview()) {
                if (!geometry) return true;
                descriptor.lodLevel=kPreviewLod;
                descriptor.chunkId |= kPreviewIdentity;
                descriptor.dependencyCount=0;
                std::fill(std::begin(descriptor.dependencyIds),std::end(descriptor.dependencyIds),0);
            } else if (sink_->Refinement()) {
                if (!geometry) return true; // immutable dependency catalog was delivered during scan
            } else if (geometry) {
                const uint64_t count = descriptor.topology == ChunkTopology::PointList ? descriptor.vertexCount : descriptor.indexCount/3;
                validPrimitives_ += count;
                const uint32_t quota = uint32_t(count < 20 ? (validPrimitives_<20 ? count : 1) : (std::min)(count/20,(std::max)(8ull,count/256)));
                // Reserve before sampling; payload + single-cluster candidates are bounded.
                const uint64_t stride=VertexStrideForLayout(VertexLayoutId(descriptor.vertexLayoutId));
                const uint64_t reserve = uint64_t(quota) * (descriptor.topology == ChunkTopology::PointList ? stride : 3*stride+12);
                if (reserve > kCoarseReservedBytes - coarseBytes_) return Fail(ImportErrorCode::ResourceLimit);
                auto sample = SampleCoarse(descriptor,a,b,quota,[this] { return sink_->Cancelled(); });
                if (!sample.Primitives()) return Fail(ImportErrorCode::Cancelled);
                coarseBytes_ += sample.vertices.size()+sample.indices.size()*4;
                samples_.push_back(std::move(sample));
                descriptor.lodLevel = kScanLod;
                descriptor.chunkId |= kScanIdentity;
                scanSummary.emplace();
                scanSummary->fullPayloadBytes = a.size() + uint64_t(b.size());
                scanSummary->fullPayloadChecksum = Fnv1a64Append(Fnv1a64(a), b);
                std::copy(std::begin(descriptor.localMin), std::end(descriptor.localMin),
                          std::begin(scanSummary->localMin));
                std::copy(std::begin(descriptor.localMax), std::end(descriptor.localMax),
                          std::begin(scanSummary->localMax));
            }
        }
        if (sink_ && sink_->RequestedSource() && geometry) {
            const auto& source = *sink_->RequestedSource();
            descriptor.chunkId = source.chunkId & ~model_core::kScanIdentity;
            descriptor.dependencyCount = source.dependencyCount;
            std::copy(std::begin(source.dependencyIds), std::end(source.dependencyIds), std::begin(descriptor.dependencyIds));
        }
        if (scanSummary) {
            descriptor.chunkChecksum = scanSummary->fullPayloadChecksum;
            return AddRaw(descriptor, std::as_bytes(std::span(&*scanSummary, 1)), {},
                          descriptor.chunkChecksum);
        }
        return AddRaw(descriptor,a,b);
    }
    bool Complete() {
        using namespace model_core;
        if (sink_ && sink_->ProxyEnabled() && !sink_->Preview() && !sink_->Refinement()) {
            // ADR-015 defines the mandatory region floor for tiny components;
            // the absolute primitive and reserved-byte ceilings remain hard.
            const uint64_t cap=CoarsePrimitiveCap(validPrimitives_,samples_.size());
            if (!cap || samples_.size()>cap) return Fail(ImportErrorCode::ResourceLimit);
            // Start coarse delivery in scene-wide source strata. The first
            // bounded section spans the catalog instead of exhausting its prefix.
            std::vector<size_t> order;
            std::vector<bool> chosen(samples_.size());
            for (size_t stratum=0;stratum<8;++stratum) {
                const size_t index=(stratum*samples_.size()+samples_.size()/2)/8;
                if (index<samples_.size() && !chosen[index]) { order.push_back(index); chosen[index]=true; }
            }
            for (size_t index=0;index<samples_.size();++index) if (!chosen[index]) order.push_back(index);
            uint64_t remaining=cap, primitives=0, bytes=0;
            for (size_t i=0;i<samples_.size();++i) {
                auto& sample=samples_[order[i]];
                const uint64_t quota=(std::min)(sample.Primitives(),remaining-(samples_.size()-i-1));
                if (quota<sample.Primitives()) {
                    auto d=sample.descriptor; d.chunkId &= ~kCoarseIdentity;
                    sample=SampleCoarse(d,sample.vertices,std::as_bytes(std::span(sample.indices)),uint32_t(quota));
                }
                primitives+=sample.Primitives(); remaining-=sample.Primitives();
                bytes+=sample.vertices.size()+sample.indices.size()*4;
                if (!AddRaw(sample.descriptor,sample.vertices,std::as_bytes(std::span(sample.indices)))) return false;
            }
            if (!CommitCoarseBudget()) return false;
            CoarseCompletePayload complete{uint32_t(samples_.size()),0,primitives,bytes};
            ChunkDescriptor d{}; d.topology=ChunkTopology::CoarseComplete; d.chunkId=0xf0000002u;
            if (!AddRaw(d,std::as_bytes(std::span(&complete,1)))) return false;
        }
        Finalize();
        return true;
    }
  private:
    bool AddRaw(model_core::ChunkDescriptor descriptor, std::span<const std::byte> a,
                std::span<const std::byte> b = {}, std::optional<uint64_t> retainedChecksum = {})
    {
        using namespace model_core;
        const uint64_t bytes = a.size() + uint64_t(b.size());
        if (catalog_.size() >= kTierACatalogLimit)
            return Fail(ImportErrorCode::ChunkCatalogLimit);
        if ((catalog_.size() + 1) * sizeof(ChunkDescriptor) > TierAScratchLimit() / 4)
            return Fail(ImportErrorCode::ScratchLimit);
        if (bytes > output_.size() || kSectionHeaderSize + kChunkDescriptorSize + bytes > output_.size())
            return Fail(ImportErrorCode::ResourceLimit);
        if (descriptors_.size() >= maxChunks_ || length_ + kChunkDescriptorSize + bytes > output_.size())
        {
            if (!sink_)
                return Fail(ImportErrorCode::ResourceLimit);
            if (!CommitCoarseBudget()) return false;
            Finalize();
            if (!sink_->PublishBatch(Count(), length_))
                return Fail(ImportErrorCode::ImportProtocolViolation);
            descriptors_.clear();
            length_ = kSectionHeaderSize;
        }
        // Growing the descriptor table moves only the bounded current window.
        const size_t oldStart = kSectionHeaderSize + descriptors_.size() * kChunkDescriptorSize;
        std::memmove(output_.data() + oldStart + kChunkDescriptorSize, output_.data() + oldStart,
                     size_t(length_ - oldStart));
        for (auto& old : descriptors_)
            old.normalizedRangeOffset += kChunkDescriptorSize;
        length_ += kChunkDescriptorSize;
        descriptor.normalizedRangeOffset = length_;
        descriptor.normalizedRangeLength = bytes;
        descriptor.byteSize = bytes;
        if (!a.empty())
            std::memcpy(output_.data() + length_, a.data(), a.size());
        if (!b.empty())
            std::memcpy(output_.data() + length_ + a.size(), b.data(), b.size());
        descriptor.chunkChecksum = retainedChecksum.value_or(
            Fnv1a64(output_.subspan(size_t(length_), size_t(bytes))));
        if (descriptor.lodLevel==kCoarseLod) { coarseVertexBatch_+=a.size(); coarseIndexBatch_+=b.size(); }
        descriptors_.push_back(descriptor);
        catalog_.push_back(descriptor);
        length_ += bytes;
        return true;
    }
  public:
    bool PublishPending()
    {
        if (!Count())
            return true;
        if (!sink_)
            return Fail(model_core::ImportErrorCode::ResourceLimit);
        if (!CommitCoarseBudget()) return false;
        Finalize();
        if (!sink_->PublishBatch(Count(), length_))
            return Fail(model_core::ImportErrorCode::ImportProtocolViolation);
        descriptors_.clear();
        length_ = model_core::kSectionHeaderSize;
        return true;
    }
    void Finalize()
    {
        using namespace model_core;
        for (size_t i = 0; i < descriptors_.size(); ++i)
            std::memcpy(output_.data() + kSectionHeaderSize + i * kChunkDescriptorSize, &descriptors_[i],
                        kChunkDescriptorSize);
        SectionHeader header{};
        header.magic = kSectionMagic;
        header.protocolVersion = kCurrentProtocolVersion;
        header.generationId = generation_;
        header.scene = scene_;
        header.scene.generationId = generation_;
        header.chunkCount = Count();
        header.sectionLength = length_;
        header.sectionChecksum =
            Fnv1a64(output_.subspan(kSectionHeaderSize, size_t(length_ - kSectionHeaderSize)));
        std::memcpy(output_.data(), &header, sizeof(header));
    }
    uint32_t Count() const
    {
        return uint32_t(descriptors_.size());
    }
    uint64_t Length() const
    {
        return length_;
    }
    model_core::ImportErrorCode Error() const
    {
        return error_;
    }
    uint32_t NextId() const
    {
        return uint32_t(catalog_.size() + 1);
    }

  private:
    bool CommitCoarseBudget() {
        auto aligned=[](uint64_t bytes) { return (bytes+65535)/65536*65536; };
        coarseAllocationBytes_+=aligned(coarseVertexBatch_)+aligned(coarseIndexBatch_);
        coarseVertexBatch_=coarseIndexBatch_=0;
        return coarseAllocationBytes_<=model_core::kCoarseReservedBytes || Fail(model_core::ImportErrorCode::ResourceLimit);
    }
    bool Fail(model_core::ImportErrorCode error)
    {
        error_ = error;
        return false;
    }
    std::span<std::byte> output_;
    uint64_t generation_;
    uint32_t maxChunks_;
    model_core::SceneMetadata scene_;
    ChunkBatchSink* sink_;
    uint64_t length_ = model_core::kSectionHeaderSize;
    std::vector<model_core::ChunkDescriptor> descriptors_;
    std::deque<model_core::ChunkDescriptor> catalog_;
    model_core::ImportErrorCode error_ = model_core::ImportErrorCode::None;
    uint64_t validPrimitives_=0, coarseBytes_=0;
    uint64_t coarseVertexBatch_=0,coarseIndexBatch_=0,coarseAllocationBytes_=0;
    std::vector<CoarseSample> samples_;
};
template <class T> std::span<const std::byte> ChunkBytes(const std::vector<T>& values)
{
    return {reinterpret_cast<const std::byte*>(values.data()), values.size() * sizeof(T)};
}
template <class T> std::span<const std::byte> ChunkBytes(const T& value)
{
    return {reinterpret_cast<const std::byte*>(&value), sizeof(T)};
}
constexpr uint64_t kTierAPrimaryBytes = 8ull * 1024 * 1024 * 1024;
constexpr uint64_t kTierAAllSourceBytes = 12ull * 1024 * 1024 * 1024;
constexpr uint64_t kTierATriangles = 100'000'000;
constexpr uint64_t kTierAPoints = 100'000'000;
constexpr uint64_t kTierAVertices = 300'000'000;
constexpr uint32_t kChunkTriangles =
    model_core::kTierAClusterTriangles; // 6.75 MiB worst-case deindexed triangle payload
constexpr uint32_t kChunkPoints = model_core::kTierAClusterPoints; // 6 MiB position-only payload
inline uint64_t TierAScratchLimit()
{
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    return GlobalMemoryStatusEx(&memory) ? (std::min)(1ull * 1024 * 1024 * 1024, memory.ullTotalPhys / 4) : 0;
}
} // namespace import_worker
