#pragma once
#include "ChunkBatchSink.h"
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

namespace import_worker
{
inline uint64_t TierAScratchLimit();
// Payload is copied into the reusable output window immediately. No normalized
// geometry survives Add; only a fixed-width source catalog survives publication.
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
        descriptor.chunkChecksum = Fnv1a64(output_.subspan(size_t(length_), size_t(bytes)));
        descriptors_.push_back(descriptor);
        catalog_.push_back(descriptor);
        length_ += bytes;
        return true;
    }
    bool PublishPending()
    {
        if (!Count())
            return true;
        if (!sink_)
            return Fail(model_core::ImportErrorCode::ResourceLimit);
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
