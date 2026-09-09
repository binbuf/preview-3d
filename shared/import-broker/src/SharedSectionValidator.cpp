#include "import_broker/SharedSectionValidator.h"

#include "model_core/Checksum.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"
#include "platform/CheckedMath.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <unordered_map>

namespace import_broker {

namespace {

using model_core::AlphaModeId;
using model_core::ChunkDescriptor;
using model_core::ChunkTopology;
using model_core::ColorSpaceId;
using model_core::ComputeImagePixelBytes;
using model_core::Fnv1a64;
using model_core::ImagePayloadHeader;
using model_core::ImportErrorCode;
using model_core::kMaterialFlagsKnownMask;
using model_core::MaterialPayload;
using model_core::PixelFormatBlockInfo;
using model_core::PixelFormatId;
using model_core::SectionHeader;
using model_core::VertexStrideForLayout;
using platform::CheckedAdd;
using platform::CheckedMultiply;

// Sanity cap bounding the mip-level loop in ComputeImagePixelBytes; not
// itself part of the wire format, purely a validator-side guard.
constexpr uint32_t kMaxImageMipLevels = 16;

// Decoded-texture-pixel budget (Tier A) per
// .docs/design/03-file-formats-and-ingestion.md. Re-enforced here
// independent of whatever the worker itself enforced -- this validator must
// never rely on worker self-restraint.
constexpr uint64_t kMaxAggregateDecodedTexturePixels = 1'000'000'000;

ValidationResult Reject(ImportErrorCode code, std::string message)
{
    ValidationResult result;
    result.ok = false;
    result.errorCode = code;
    result.diagnosticMessage = std::move(message);
    return result;
}

bool AllFinite(std::initializer_list<float> values)
{
    for (float v : values) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

} // namespace

ValidationResult ValidateAndCopySection(std::span<const std::byte> sectionView,
                                         uint64_t expectedGenerationId, uint32_t maxChunkCount)
{
    // 1. The section must be at least large enough to hold a header before
    // any field of it is read.
    if (sectionView.size() < sizeof(SectionHeader)) {
        return Reject(ImportErrorCode::MalformedData, "section smaller than header");
    }

    // 2. Copy the header into a local, private copy -- validate the copy,
    // never the shared view directly, even on the honest path.
    SectionHeader header{};
    std::memcpy(&header, sectionView.data(), sizeof(header));

    // 3.
    if (header.magic != model_core::kSectionMagic) {
        return Reject(ImportErrorCode::MalformedData, "bad magic");
    }

    // 4. Never attempt to interpret an unrecognized protocol version.
    if (header.protocolVersion != model_core::kCurrentProtocolVersion) {
        return Reject(ImportErrorCode::ImportProtocolViolation, "unrecognized protocol version");
    }

    // 5. Bounds-check the header's self-declared length against the actual
    // mapped size before trusting it for anything else.
    if (header.sectionLength > sectionView.size()) {
        return Reject(ImportErrorCode::MalformedData, "sectionLength exceeds mapped view");
    }

    // 6.
    if (header.sectionLength < sizeof(SectionHeader)) {
        return Reject(ImportErrorCode::MalformedData, "sectionLength smaller than header");
    }

    // 7. Sane cap enforced before any allocation sized by chunkCount.
    if (header.chunkCount > maxChunkCount) {
        return Reject(ImportErrorCode::ResourceLimit, "chunkCount exceeds cap");
    }

    // 8. Descriptor table must fit within the already-bounds-checked
    // section length, via checked arithmetic.
    auto descriptorTableBytes
        = CheckedMultiply(static_cast<uint64_t>(header.chunkCount), model_core::kChunkDescriptorSize);
    if (!descriptorTableBytes) {
        return Reject(ImportErrorCode::MalformedData, "chunkCount * descriptor size overflows");
    }
    auto needed = CheckedAdd(model_core::kSectionHeaderSize, *descriptorTableBytes);
    if (!needed) {
        return Reject(ImportErrorCode::MalformedData, "header + descriptor table size overflows");
    }

    // 9.
    if (*needed > header.sectionLength) {
        return Reject(ImportErrorCode::MalformedData, "descriptor table exceeds sectionLength");
    }

    // 9.5. Bulk-copy the entire live section into host-owned private memory
    // in ONE read, now that [0, header.sectionLength) is provably within
    // sectionView (checks 5+6+9 above). sectionView is adversary-writable
    // for as long as the caller keeps it mapped; every check and copy from
    // this point on reads only this private snapshot, never sectionView
    // again -- eliminating the TOCTOU window between "checked something"
    // and "read it again to copy/verify it."
    std::vector<std::byte> sectionCopy(sectionView.begin(),
                                        sectionView.begin() + header.sectionLength);
    std::span<const std::byte> section(sectionCopy);

    // 10. Recompute the section checksum over [header, sectionLength).
    auto payloadRegion
        = section.subspan(sizeof(SectionHeader), header.sectionLength - sizeof(SectionHeader));
    uint64_t recomputedChecksum = Fnv1a64(payloadRegion);
    if (recomputedChecksum != header.sectionChecksum) {
        return Reject(ImportErrorCode::MalformedData, "section checksum mismatch");
    }

    // 11. Staleness guard.
    if (header.generationId != expectedGenerationId) {
        return Reject(ImportErrorCode::ImportProtocolViolation, "generation ID mismatch");
    }

    // 12. Pass A: copy every descriptor and build a chunkId -> topology map
    // before any dependency reference is validated -- a mesh/material chunk
    // may depend on a chunk that appears later in the table, so the whole
    // table must be known before Pass B below can resolve any dependency.
    std::vector<ChunkDescriptor> descriptors(header.chunkCount);
    std::unordered_map<uint32_t, ChunkTopology> topologyById;
    topologyById.reserve(header.chunkCount);
    for (uint32_t i = 0; i < header.chunkCount; ++i) {
        size_t descriptorOffset = sizeof(SectionHeader) + i * model_core::kChunkDescriptorSize;
        std::memcpy(&descriptors[i], section.data() + descriptorOffset, sizeof(ChunkDescriptor));

        // 12a. chunkId 0 is the "no dependency" sentinel -- a chunk must not
        // claim it as its own identity.
        if (descriptors[i].chunkId == 0) {
            return Reject(ImportErrorCode::MalformedData, "chunk declares reserved chunkId 0");
        }
        auto [it, inserted] = topologyById.emplace(descriptors[i].chunkId, descriptors[i].topology);
        if (!inserted) {
            return Reject(ImportErrorCode::MalformedData, "duplicate chunkId");
        }
        (void)it;
    }

    ValidationResult result;
    result.chunks.reserve(header.chunkCount);
    uint64_t aggregateImagePixels = 0;

    for (uint32_t i = 0; i < header.chunkCount; ++i) {
        const ChunkDescriptor& descriptor = descriptors[i];

        // 12b. Applies to every topology.
        auto payloadEnd = CheckedAdd(descriptor.normalizedRangeOffset, descriptor.byteSize);
        if (!payloadEnd || *payloadEnd > header.sectionLength) {
            return Reject(ImportErrorCode::MalformedData, "chunk payload exceeds sectionLength");
        }

        // 12c.
        if (descriptor.byteSize != descriptor.normalizedRangeLength) {
            return Reject(ImportErrorCode::MalformedData, "byteSize/normalizedRangeLength mismatch");
        }

        // 12d. Pass B: topology-specific validation. Every branch either
        // Rejects or falls through to the common checksum/copy tail below.
        switch (descriptor.topology) {
        case ChunkTopology::TriangleList:
        case ChunkTopology::PointList: {
            // Unrecognized layout ID is never guessed.
            size_t stride = VertexStrideForLayout(
                static_cast<model_core::VertexLayoutId>(descriptor.vertexLayoutId));
            if (stride == 0) {
                return Reject(ImportErrorCode::MalformedData, "unrecognized vertex layout ID");
            }

            // Expected byte size from vertex/index counts, via checked
            // arithmetic. PointList chunks must declare indexCount == 0 --
            // no index buffer for points, per this chunk's scope.
            auto vertexBytes = CheckedMultiply(static_cast<uint64_t>(descriptor.vertexCount), stride);
            if (!vertexBytes) {
                return Reject(ImportErrorCode::MalformedData, "vertexCount * stride overflows");
            }

            uint64_t expectedByteSize = 0;
            if (descriptor.topology == ChunkTopology::TriangleList) {
                auto indexBytes = CheckedMultiply(static_cast<uint64_t>(descriptor.indexCount),
                                                   static_cast<uint64_t>(sizeof(uint32_t)));
                if (!indexBytes) {
                    return Reject(ImportErrorCode::MalformedData, "indexCount * 4 overflows");
                }
                auto total = CheckedAdd(*vertexBytes, *indexBytes);
                if (!total) {
                    return Reject(ImportErrorCode::MalformedData, "vertex + index bytes overflows");
                }
                expectedByteSize = *total;
            } else {
                if (descriptor.indexCount != 0) {
                    return Reject(ImportErrorCode::MalformedData, "PointList chunk declares indices");
                }
                expectedByteSize = *vertexBytes;
            }

            if (expectedByteSize != descriptor.byteSize) {
                return Reject(ImportErrorCode::MalformedData, "byteSize does not match declared counts");
            }

            // dependencyIds on a TriangleList/PointList chunk predates this
            // chunk's Material/Image work -- SyntheticSceneGenerator.cpp
            // already uses it for a PointList-depends-on-TriangleList
            // LOD/derivation relationship, unrelated to materials. Only
            // existence is validated here, not one fixed target topology; a
            // mesh-to-Material link (this chunk's own addition, written by
            // GltfAdapter.cpp) is simply one populated slot whose target
            // happens to have Material topology -- whoever interprets a
            // specific slot's meaning (e.g. D3D12ImportBridge, host-side)
            // checks the target chunk's own topology rather than assuming a
            // fixed slot meaning.
            if (descriptor.dependencyCount > model_core::kMaxDependencyIds) {
                return Reject(ImportErrorCode::MalformedData, "mesh dependencyCount exceeds cap");
            }
            for (uint32_t d = 0; d < descriptor.dependencyCount; ++d) {
                if (descriptor.dependencyIds[d] == 0
                    || topologyById.find(descriptor.dependencyIds[d]) == topologyById.end()) {
                    return Reject(ImportErrorCode::MalformedData, "mesh dependency id not found");
                }
            }
            for (uint32_t d = descriptor.dependencyCount; d < model_core::kMaxDependencyIds; ++d) {
                if (descriptor.dependencyIds[d] != 0) {
                    return Reject(ImportErrorCode::MalformedData,
                                  "mesh declares an id in an unpopulated dependency slot");
                }
            }
            break;
        }
        case ChunkTopology::Material: {
            if (descriptor.vertexCount != 0 || descriptor.indexCount != 0
                || descriptor.vertexLayoutId != 0) {
                return Reject(ImportErrorCode::MalformedData,
                              "material chunk declares nonzero geometry fields");
            }
            if (descriptor.byteSize != sizeof(MaterialPayload)) {
                return Reject(ImportErrorCode::MalformedData, "material payload size mismatch");
            }

            MaterialPayload payload{};
            std::memcpy(&payload, section.data() + descriptor.normalizedRangeOffset, sizeof(payload));

            if (!AllFinite({ payload.baseColorFactor[0], payload.baseColorFactor[1],
                              payload.baseColorFactor[2], payload.baseColorFactor[3],
                              payload.metallicFactor, payload.roughnessFactor,
                              payload.emissiveFactor[0], payload.emissiveFactor[1],
                              payload.emissiveFactor[2], payload.uvOffset[0], payload.uvOffset[1],
                              payload.uvScale[0], payload.uvScale[1], payload.uvRotation,
                              payload.alphaCutoff })) {
                return Reject(ImportErrorCode::MalformedData, "material payload contains a non-finite value");
            }
            if (payload.alphaMode > static_cast<uint32_t>(AlphaModeId::Blend)) {
                return Reject(ImportErrorCode::MalformedData, "material alphaMode out of range");
            }
            if ((payload.flags & ~kMaterialFlagsKnownMask) != 0) {
                return Reject(ImportErrorCode::MalformedData, "material flags has unrecognized bits set");
            }
            if (descriptor.dependencyCount > model_core::kMaxDependencyIds) {
                return Reject(ImportErrorCode::MalformedData, "material dependencyCount exceeds cap");
            }
            for (uint32_t d = 0; d < descriptor.dependencyCount; ++d) {
                auto dep = topologyById.find(descriptor.dependencyIds[d]);
                if (descriptor.dependencyIds[d] == 0 || dep == topologyById.end()
                    || dep->second != ChunkTopology::Image) {
                    return Reject(ImportErrorCode::MalformedData, "material dependency is not an image chunk");
                }
            }
            for (uint32_t d = descriptor.dependencyCount; d < model_core::kMaxDependencyIds; ++d) {
                if (descriptor.dependencyIds[d] != 0) {
                    return Reject(ImportErrorCode::MalformedData,
                                  "material declares an id in an unpopulated dependency slot");
                }
            }
            break;
        }
        case ChunkTopology::Image: {
            if (descriptor.vertexCount != 0 || descriptor.indexCount != 0
                || descriptor.vertexLayoutId != 0) {
                return Reject(ImportErrorCode::MalformedData, "image chunk declares nonzero geometry fields");
            }
            // Images reference nothing -- this makes the mesh -> material ->
            // image graph acyclic by construction; no cycle detection needed.
            if (descriptor.dependencyCount != 0) {
                return Reject(ImportErrorCode::MalformedData, "image chunk must not declare dependencies");
            }
            for (uint32_t d = 0; d < model_core::kMaxDependencyIds; ++d) {
                if (descriptor.dependencyIds[d] != 0) {
                    return Reject(ImportErrorCode::MalformedData, "image chunk declares a nonzero dependency slot");
                }
            }
            if (descriptor.byteSize < sizeof(ImagePayloadHeader)) {
                return Reject(ImportErrorCode::MalformedData, "image payload smaller than header");
            }

            ImagePayloadHeader imageHeader{};
            std::memcpy(&imageHeader, section.data() + descriptor.normalizedRangeOffset, sizeof(imageHeader));

            if (imageHeader.reserved0 != 0) {
                return Reject(ImportErrorCode::MalformedData, "image payload reserved0 must be 0");
            }
            auto pixelFormat = static_cast<PixelFormatId>(imageHeader.pixelFormat);
            if (!PixelFormatBlockInfo(pixelFormat)) {
                return Reject(ImportErrorCode::MalformedData, "unrecognized image pixel format");
            }
            if (imageHeader.colorSpace != static_cast<uint32_t>(ColorSpaceId::Linear)
                && imageHeader.colorSpace != static_cast<uint32_t>(ColorSpaceId::Srgb)) {
                return Reject(ImportErrorCode::MalformedData, "unrecognized image color space");
            }
            // BC5_UNORM has no DXGI _SRGB variant -- catch the
            // unrepresentable combination here rather than at upload time.
            if (pixelFormat == PixelFormatId::BC5_UNORM
                && imageHeader.colorSpace == static_cast<uint32_t>(ColorSpaceId::Srgb)) {
                return Reject(ImportErrorCode::MalformedData, "BC5_UNORM has no sRGB representation");
            }
            if (imageHeader.width == 0 || imageHeader.height == 0) {
                return Reject(ImportErrorCode::MalformedData, "image declares zero width or height");
            }
            if (imageHeader.mipLevels == 0 || imageHeader.mipLevels > kMaxImageMipLevels) {
                return Reject(ImportErrorCode::MalformedData, "image mipLevels out of range");
            }

            auto expectedPixelBytes = ComputeImagePixelBytes(pixelFormat, imageHeader.width,
                                                               imageHeader.height, imageHeader.mipLevels);
            if (!expectedPixelBytes || *expectedPixelBytes != imageHeader.pixelDataByteSize) {
                return Reject(ImportErrorCode::MalformedData,
                              "image pixelDataByteSize does not match declared dimensions");
            }
            auto expectedTotal = CheckedAdd(static_cast<uint64_t>(sizeof(ImagePayloadHeader)),
                                             *expectedPixelBytes);
            if (!expectedTotal || *expectedTotal != descriptor.byteSize) {
                return Reject(ImportErrorCode::MalformedData,
                              "image byteSize does not match header + pixel data");
            }

            // Aggregate decoded-pixel budget (mip 0 only), re-enforced here
            // independent of the worker's own accounting.
            auto pixelCount = CheckedMultiply(static_cast<uint64_t>(imageHeader.width),
                                               static_cast<uint64_t>(imageHeader.height));
            auto newAggregate = pixelCount ? CheckedAdd(aggregateImagePixels, *pixelCount) : std::nullopt;
            if (!pixelCount || !newAggregate) {
                return Reject(ImportErrorCode::ResourceLimit, "aggregate decoded texture pixel count overflows");
            }
            aggregateImagePixels = *newAggregate;
            break;
        }
        default:
            return Reject(ImportErrorCode::MalformedData, "unrecognized topology");
        }

        // 12h. Recompute the per-chunk checksum over the validated payload
        // range. Common to every topology.
        auto chunkPayloadView
            = section.subspan(descriptor.normalizedRangeOffset, descriptor.byteSize);
        uint64_t recomputedChunkChecksum = Fnv1a64(chunkPayloadView);
        if (recomputedChunkChecksum != descriptor.chunkChecksum) {
            return Reject(ImportErrorCode::MalformedData, "chunk checksum mismatch");
        }

        // 12i. Only now, copy the payload into host-owned private memory.
        std::vector<std::byte> payload(chunkPayloadView.begin(), chunkPayloadView.end());
        result.chunks.push_back(ValidatedChunk{ descriptor, std::move(payload) });
    }

    // 13. Aggregate decoded-texture-pixel budget (1 gigapixel, Tier A),
    // enforced across the whole batch after every chunk's own checks pass.
    if (aggregateImagePixels > kMaxAggregateDecodedTexturePixels) {
        return Reject(ImportErrorCode::ResourceLimit, "aggregate decoded texture pixel budget exceeded");
    }

    // 14. Every chunk passed.
    result.ok = true;
    result.errorCode = ImportErrorCode::None;
    return result;
}

} // namespace import_broker
