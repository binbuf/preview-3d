#include "model_core/TierALimits.h"
#include "import_broker/SharedSectionValidator.h"

#include "model_core/Checksum.h"
#include "model_core/MaterialPayload.h"
#include "model_core/PixelFormats.h"
#include "model_core/VertexLayouts.h"
#include "model_core/GeometryBounds.h"
#include "platform/CheckedMath.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <functional>
#include <limits>
#include <unordered_map>

namespace import_broker {

namespace {

using model_core::AlphaModeId;
using model_core::ChunkDescriptor;
using model_core::ChunkTopology;
using model_core::ColorSpaceId;
using model_core::ComputeImagePixelBytes;
using model_core::WireChecksum64;
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

bool FiniteAffine(const double matrix[16])
{
    for (uint32_t i = 0; i < 16; ++i) {
        if (!std::isfinite(matrix[i]) || std::abs(matrix[i]) > 1e30)
            return false;
    }
    if (matrix[3] != 0.0 || matrix[7] != 0.0 || matrix[11] != 0.0 || matrix[15] != 1.0)
        return false;
    const double determinant =
        matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9])
        - matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8])
        + matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
    return std::isfinite(determinant) && std::abs(determinant) >= 1e-18;
}

bool MultiplyAffine(const double left[16], const double right[16], double out[16])
{
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            double value = 0;
            for (uint32_t k = 0; k < 4; ++k)
                value += left[row * 4 + k] * right[k * 4 + column];
            if (!std::isfinite(value) || std::abs(value) > 1e30)
                return false;
            out[row * 4 + column] = value;
        }
    }
    return FiniteAffine(out);
}

bool BoundsEqual(double actual, double expected)
{
    const double tolerance = (std::max)(1e-8, std::abs(expected) * 1e-12);
    return std::abs(actual - expected) <= tolerance;
}

bool TransformGeometryBounds(const model_core::ChunkDescriptor& geometry, const double world[16],
                             double minimum[3], double maximum[3])
{
    minimum[0] = minimum[1] = minimum[2] = (std::numeric_limits<double>::max)();
    maximum[0] = maximum[1] = maximum[2] = -(std::numeric_limits<double>::max)();
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const double point[3] = {
            geometry.origin[0] + (corner & 1 ? geometry.localMax[0] : geometry.localMin[0]),
            geometry.origin[1] + (corner & 2 ? geometry.localMax[1] : geometry.localMin[1]),
            geometry.origin[2] + (corner & 4 ? geometry.localMax[2] : geometry.localMin[2]),
        };
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const double value = point[0] * world[axis] + point[1] * world[4 + axis]
                + point[2] * world[8 + axis] + world[12 + axis];
            if (!std::isfinite(value) || std::abs(value) > 1e30)
                return false;
            minimum[axis] = (std::min)(minimum[axis], value);
            maximum[axis] = (std::max)(maximum[axis], value);
        }
    }
    return true;
}

} // namespace

ValidationResult ValidateAndCopySection(std::span<const std::byte> sectionView,
                                         uint64_t expectedGenerationId, uint32_t maxChunkCount,
                                         const KnownChunkCatalog* priorBatches, bool allowForwardReferences,
                                         const KnownImageCatalog* priorImages,
                                         uint64_t priorTextureBytes, uint64_t priorTexturePixels,
                                         KnownSceneCatalog* sceneCatalog)
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
    // Never combine a raced header with a different private payload snapshot.
    if (std::memcmp(section.data(), &header, sizeof(header)) != 0)
        return Reject(ImportErrorCode::ImportProtocolViolation, "header changed during copy");
    if (header.reserved || header.scene.reserved || header.scene.generationId != expectedGenerationId)
        return Reject(ImportErrorCode::ImportProtocolViolation, "stale or malformed scene metadata");
    if (uint32_t(header.scene.format) > uint32_t(model_core::SourceFormatId::Step))
        return Reject(ImportErrorCode::MalformedData, "invalid scene metadata");
    const bool usdFormat = header.scene.format == model_core::SourceFormatId::Usda
        || header.scene.format == model_core::SourceFormatId::Usdc
        || header.scene.format == model_core::SourceFormatId::Usdz
        || header.scene.format == model_core::SourceFormatId::ThreeMf;
    const bool tierBFormat = header.scene.format == model_core::SourceFormatId::AsciiStl
        || header.scene.format == model_core::SourceFormatId::AsciiPly
        || header.scene.format == model_core::SourceFormatId::Obj
        || header.scene.format == model_core::SourceFormatId::Fbx
        || header.scene.format == model_core::SourceFormatId::Step
        || usdFormat;
    const uint32_t objectLimit = tierBFormat ? model_core::kTierBObjectLimit
                                             : model_core::kTierAObjectLimit;
    if (uint32_t(header.scene.upAxis) > uint32_t(model_core::UpAxisId::X) ||
        !std::isfinite(header.scene.metersPerUnit) || header.scene.metersPerUnit < 0 ||
        header.scene.metersPerUnit > 1e12 || header.scene.meshCount > objectLimit ||
        header.scene.nodeCount > objectLimit || header.scene.skinCount > 1'000'000 ||
        header.scene.animationCount > 1'000'000 || header.scene.boneCount > 1'000'000)
        return Reject(ImportErrorCode::MalformedData, "invalid scene metadata");
    if ((header.scene.format == model_core::SourceFormatId::Gltf || header.scene.format == model_core::SourceFormatId::Glb)
        && (header.scene.upAxis != model_core::UpAxisId::Y || header.scene.metersPerUnit != 1.0))
        return Reject(ImportErrorCode::MalformedData, "invalid glTF units/up axis");
    if ((header.scene.format == model_core::SourceFormatId::Stl
         || header.scene.format == model_core::SourceFormatId::Ply
         || header.scene.format == model_core::SourceFormatId::AsciiStl
         || header.scene.format == model_core::SourceFormatId::AsciiPly
         || header.scene.format == model_core::SourceFormatId::Obj)
        && (header.scene.upAxis != model_core::UpAxisId::Unknown || header.scene.metersPerUnit != 0))
        return Reject(ImportErrorCode::MalformedData, "unspecified source units/up axis must stay unknown");
    if (header.scene.format == model_core::SourceFormatId::Fbx
        && (header.scene.upAxis != model_core::UpAxisId::Y || header.scene.metersPerUnit != 1.0))
        return Reject(ImportErrorCode::MalformedData, "invalid normalized FBX units/up axis");
    if (usdFormat
        && (header.scene.upAxis == model_core::UpAxisId::Unknown
            || header.scene.metersPerUnit <= 0.0))
        return Reject(ImportErrorCode::MalformedData, "USD units/up axis must be specified");
    // STEP declares no universal display-up axis, so Unknown is required, but
    // the authored length unit must convert to a verified positive metre
    // factor -- an absent/zero unit is a typed failure, never an assumed
    // millimetre.
    if (header.scene.format == model_core::SourceFormatId::Step
        && (header.scene.upAxis != model_core::UpAxisId::Unknown
            || header.scene.metersPerUnit <= 0.0))
        return Reject(ImportErrorCode::MalformedData, "STEP requires unknown up axis and a positive metre factor");

    // 10. Recompute the section checksum over [header, sectionLength).
    auto payloadRegion
        = section.subspan(sizeof(SectionHeader), header.sectionLength - sizeof(SectionHeader));
    uint64_t recomputedChecksum = WireChecksum64(payloadRegion);
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
        const auto& bounded = descriptors[i];
        if ((bounded.topology == model_core::ChunkTopology::TriangleList ||
             bounded.topology == model_core::ChunkTopology::PointList) &&
            (bounded.byteSize > 16ull * 1024 * 1024 || bounded.vertexCount > 1048576 ||
             (bounded.topology == model_core::ChunkTopology::TriangleList &&
              bounded.indexCount > 262144u * 3)))
            return Reject(ImportErrorCode::ResourceLimit, "normalized geometry cluster limit");
        if (descriptors[i].chunkId == 0) {
            return Reject(ImportErrorCode::MalformedData, "chunk declares reserved chunkId 0");
        }
        // 12a-bis. Uniqueness spans the whole generation, not just this
        // section. Without this a later batch could reuse an earlier batch's
        // id and silently take over what every already-accepted dependency
        // reference to that id resolves to.
        if (priorBatches && priorBatches->find(descriptors[i].chunkId) != priorBatches->end()) {
            return Reject(ImportErrorCode::MalformedData, "chunkId already accepted in an earlier batch");
        }
        auto [it, inserted] = topologyById.emplace(descriptors[i].chunkId, descriptors[i].topology);
        if (!inserted) {
            return Reject(ImportErrorCode::MalformedData, "duplicate chunkId");
        }
        (void)it;
    }

    // Resolves a dependency id against this section's own table first, then
    // the chunks accepted in earlier batches of this generation.
    //
    // Cycles stay impossible for the same structural reason as before: a
    // chunk can only name an id that already exists, and an earlier batch was
    // validated before this one existed, so every cross-batch reference
    // points strictly backwards in batch order.
    auto resolveTopology = [&](uint32_t id) -> const ChunkTopology* {
        auto it = topologyById.find(id);
        if (it != topologyById.end()) {
            return &it->second;
        }
        if (priorBatches) {
            auto prior = priorBatches->find(id);
            if (prior != priorBatches->end()) {
                return &prior->second;
            }
        }
        return nullptr;
    };

    ValidationResult result;
    result.chunks.reserve(header.chunkCount);
    uint64_t aggregateImagePixels = priorTexturePixels;
    uint64_t aggregateImageBytes = priorTextureBytes;
    KnownImageCatalog images = priorImages ? *priorImages : KnownImageCatalog{};
    KnownSceneCatalog scenes = sceneCatalog ? *sceneCatalog : KnownSceneCatalog{};
    std::unordered_map<uint32_t, model_core::NodePayload> pendingNodes;
    std::unordered_map<uint32_t, model_core::MeshInstancePayload> pendingInstances;

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
        if (descriptor.lodLevel > model_core::kPreviewLod)
            return Reject(ImportErrorCode::MalformedData, "unknown geometry role");
        if (descriptor.topology != ChunkTopology::TriangleList && descriptor.topology != ChunkTopology::PointList) {
            if (descriptor.lodLevel || descriptor.meshId || descriptor.nodeId || descriptor.geometryFlags || descriptor.sourceElementOffset
                || descriptor.boundsState != model_core::BoundsState::Unknown)
                return Reject(ImportErrorCode::MalformedData, "non-geometry chunk declares geometry metadata");
            for (unsigned axis=0; axis<3; ++axis) if (descriptor.origin[axis] != 0
                || descriptor.localMin[axis] != 0 || descriptor.localMax[axis] != 0)
                return Reject(ImportErrorCode::MalformedData, "non-geometry chunk declares origin/bounds");
        }

        // 12d. Pass B: topology-specific validation. Every branch either
        // Rejects or falls through to the common checksum/copy tail below.
        switch (descriptor.topology) {
        case ChunkTopology::Node: {
            if (descriptor.byteSize != sizeof(model_core::NodePayload) || descriptor.lodLevel
                || descriptor.vertexCount || descriptor.indexCount || descriptor.vertexLayoutId
                || descriptor.sourceRangeOffset || descriptor.sourceRangeLength)
                return Reject(ImportErrorCode::MalformedData, "invalid node descriptor");
            model_core::NodePayload node{};
            std::memcpy(&node, section.data() + descriptor.normalizedRangeOffset, sizeof(node));
            if (!node.nodeId || node.nodeId != descriptor.chunkId || node.reserved
                || (node.flags & ~model_core::kSceneRecordFlagsKnownMask) || !FiniteAffine(node.localTransform))
                return Reject(ImportErrorCode::MalformedData, "invalid node payload");
            const uint32_t expectedDependencies = node.parentNodeId ? 1u : 0u;
            if (descriptor.dependencyCount != expectedDependencies
                || descriptor.dependencyIds[0] != node.parentNodeId)
                return Reject(ImportErrorCode::MalformedData, "node parent slot mismatch");
            for (uint32_t d = 1; d < model_core::kMaxDependencyIds; ++d)
                if (descriptor.dependencyIds[d])
                    return Reject(ImportErrorCode::MalformedData, "node declares an unexpected dependency");
            if (node.parentNodeId) {
                const auto* topology = resolveTopology(node.parentNodeId);
                if (!topology || *topology != ChunkTopology::Node || node.parentNodeId == node.nodeId)
                    return Reject(ImportErrorCode::MalformedData, "node parent is not a node");
            }
            if (scenes.nodes.contains(node.nodeId) || !pendingNodes.emplace(node.nodeId, node).second)
                return Reject(ImportErrorCode::MalformedData, "duplicate node record");
            break;
        }
        case ChunkTopology::MeshInstance: {
            if (descriptor.byteSize != sizeof(model_core::MeshInstancePayload) || descriptor.lodLevel
                || descriptor.vertexCount || descriptor.indexCount || descriptor.vertexLayoutId
                || descriptor.sourceRangeOffset || descriptor.sourceRangeLength)
                return Reject(ImportErrorCode::MalformedData, "invalid instance descriptor");
            model_core::MeshInstancePayload instance{};
            std::memcpy(&instance, section.data() + descriptor.normalizedRangeOffset, sizeof(instance));
            if (!instance.instanceId || instance.instanceId != descriptor.chunkId || !instance.nodeId
                || !instance.geometryChunkId || instance.reserved[0] || instance.reserved[1]
                || instance.reserved[2] || (instance.flags & ~model_core::kSceneRecordFlagsKnownMask))
                return Reject(ImportErrorCode::MalformedData, "invalid instance payload");
            const uint32_t expectedCount = instance.materialChunkId ? 3u : 2u;
            if (descriptor.dependencyCount != expectedCount
                || descriptor.dependencyIds[0] != instance.geometryChunkId
                || descriptor.dependencyIds[1] != instance.materialChunkId
                || descriptor.dependencyIds[2] != instance.nodeId || descriptor.dependencyIds[3])
                return Reject(ImportErrorCode::MalformedData, "instance dependency slots mismatch");
            const auto* geometryTopology = resolveTopology(instance.geometryChunkId);
            const auto* nodeTopology = resolveTopology(instance.nodeId);
            const auto* materialTopology = instance.materialChunkId
                ? resolveTopology(instance.materialChunkId) : nullptr;
            if (!geometryTopology || (*geometryTopology != ChunkTopology::TriangleList
                                      && *geometryTopology != ChunkTopology::PointList)
                || !nodeTopology || *nodeTopology != ChunkTopology::Node
                || (instance.materialChunkId && (!materialTopology || *materialTopology != ChunkTopology::Material)))
                return Reject(ImportErrorCode::MalformedData, "instance dependency has the wrong topology");
            for (uint32_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(instance.worldMin[axis]) || !std::isfinite(instance.worldMax[axis])
                    || std::abs(instance.worldMin[axis]) > 1e30 || std::abs(instance.worldMax[axis]) > 1e30
                    || instance.worldMin[axis] > instance.worldMax[axis])
                    return Reject(ImportErrorCode::MalformedData, "invalid instance bounds");
            }
            if (scenes.instances.contains(instance.instanceId)
                || !pendingInstances.emplace(instance.instanceId, instance).second)
                return Reject(ImportErrorCode::MalformedData, "duplicate instance record");
            break;
        }
        case ChunkTopology::CoarseComplete: {
            if (descriptor.byteSize != sizeof(model_core::CoarseCompletePayload) || descriptor.lodLevel
                || descriptor.chunkId != 0xf0000002u
                || descriptor.vertexCount || descriptor.indexCount || descriptor.vertexLayoutId || descriptor.dependencyCount
                || descriptor.sourceRangeOffset || descriptor.sourceRangeLength)
                return Reject(ImportErrorCode::MalformedData,"invalid coarse completion record");
            for (auto id:descriptor.dependencyIds) if (id)
                return Reject(ImportErrorCode::MalformedData,"coarse completion has dependencies");
            model_core::CoarseCompletePayload complete{};
            std::memcpy(&complete,section.data()+descriptor.normalizedRangeOffset,sizeof(complete));
            if (!complete.regions || complete.reserved || !complete.primitives
                || complete.primitives > model_core::kCoarsePrimitiveLimit
                || !complete.geometryBytes || complete.geometryBytes > model_core::kCoarseReservedBytes)
                return Reject(ImportErrorCode::MalformedData,"invalid coarse completion totals");
            break;
        }
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

            const bool scanSummary = descriptor.lodLevel == model_core::kScanLod;
            if ((!scanSummary && expectedByteSize != descriptor.byteSize)
                || (scanSummary && descriptor.byteSize != sizeof(model_core::ScanSummaryPayload))) {
                return Reject(ImportErrorCode::MalformedData, "byteSize does not match declared counts");
            }

            if (descriptor.sourceElementOffset>252 || (descriptor.sourceElementOffset
                && ((header.scene.format!=model_core::SourceFormatId::Ply
                     && header.scene.format!=model_core::SourceFormatId::AsciiPly)
                    || descriptor.topology!=model_core::ChunkTopology::TriangleList)))
                return Reject(model_core::ImportErrorCode::ImportProtocolViolation, "invalid source fan offset");
            if (descriptor.boundsState != model_core::BoundsState::Verified
                || (descriptor.geometryFlags & ~model_core::kGeometryFlagsKnownMask) != 0
                || (descriptor.meshId && descriptor.meshId > header.scene.meshCount)
                || (descriptor.nodeId && descriptor.nodeId > header.scene.nodeCount))
                return Reject(ImportErrorCode::MalformedData, "invalid geometry metadata");
            for (unsigned axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(descriptor.origin[axis]) || std::abs(descriptor.origin[axis]) > 1e30
                    || !std::isfinite(descriptor.localMin[axis]) || !std::isfinite(descriptor.localMax[axis])
                    || descriptor.localMin[axis] > descriptor.localMax[axis]
                    || std::abs(double(descriptor.localMin[axis])) > 1e30
                    || std::abs(double(descriptor.localMax[axis])) > 1e30)
                    return Reject(ImportErrorCode::MalformedData, "non-finite or malformed origin/bounds");
            }
            if (scanSummary) {
                model_core::ScanSummaryPayload summary{};
                std::memcpy(&summary, section.data() + descriptor.normalizedRangeOffset, sizeof(summary));
                if (summary.fullPayloadBytes != expectedByteSize
                    || summary.fullPayloadChecksum != descriptor.chunkChecksum
                    || std::memcmp(summary.localMin, descriptor.localMin, sizeof(summary.localMin))
                    || std::memcmp(summary.localMax, descriptor.localMax, sizeof(summary.localMax)))
                    return Reject(ImportErrorCode::MalformedData, "invalid scan summary");
            } else {
                auto geometry = section.subspan(size_t(descriptor.normalizedRangeOffset), size_t(*vertexBytes));
                auto reduced = descriptor;
                if (!model_core::SetLocalBounds(reduced, geometry)
                    || std::memcmp(reduced.localMin, descriptor.localMin, sizeof(reduced.localMin))
                    || std::memcmp(reduced.localMax, descriptor.localMax, sizeof(reduced.localMax)))
                    return Reject(ImportErrorCode::MalformedData, "fabricated geometry bounds");
                // Check every normalized attribute, including PositionOnly_F32.
                for (size_t offset = 0; offset < geometry.size(); offset += sizeof(float)) {
                    float value; std::memcpy(&value, geometry.data() + offset, sizeof(value));
                    if (!std::isfinite(value)) return Reject(ImportErrorCode::MalformedData, "non-finite vertex");
                }
            }
            if (!scanSummary && descriptor.topology == ChunkTopology::TriangleList) {
                if (descriptor.indexCount % 3) return Reject(ImportErrorCode::MalformedData, "partial triangle");
                for (uint32_t index = 0; index < descriptor.indexCount; ++index) {
                    uint32_t value;
                    std::memcpy(&value, section.data() + descriptor.normalizedRangeOffset + *vertexBytes
                        + size_t(index) * sizeof(value), sizeof(value));
                    if (value >= descriptor.vertexCount) return Reject(ImportErrorCode::MalformedData, "invalid index");
                }
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
                    || (!allowForwardReferences && resolveTopology(descriptor.dependencyIds[d]) == nullptr)) {
                    return Reject(ImportErrorCode::MalformedData, "mesh dependency id not found");
                }
            }
            for (uint32_t d = descriptor.dependencyCount; d < model_core::kMaxDependencyIds; ++d) {
                if (descriptor.dependencyIds[d] != 0) {
                    return Reject(ImportErrorCode::MalformedData,
                                  "mesh declares an id in an unpopulated dependency slot");
                }
            }
            if (!scanSummary)
                scenes.geometry.emplace(descriptor.chunkId, descriptor);
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
            // Unlike a mesh's single optional dependency, a material's 4
            // slots carry per-position semantic meaning
            // (baseColor/metallicRoughness/normal/emissive per WireFormat.h)
            // and may be sparsely populated -- e.g. a normal-map-only
            // material has only slot 2 nonzero. Every slot is therefore
            // checked independently (0 = no texture in that slot, always
            // valid) rather than treating dependencyCount as a contiguous
            // prefix length; dependencyCount itself is cross-checked
            // against the actual count of nonzero slots so a lying worker
            // can't understate it.
            {
                uint32_t populatedCount = 0;
                for (uint32_t d = 0; d < model_core::kMaxDependencyIds; ++d) {
                    if (descriptor.dependencyIds[d] == 0) {
                        continue;
                    }
                    const ChunkTopology* dep = resolveTopology(descriptor.dependencyIds[d]);
                    if ((dep == nullptr && !allowForwardReferences) || (dep && *dep != ChunkTopology::Image)) {
                        return Reject(ImportErrorCode::MalformedData,
                                      "material dependency is not an image chunk");
                    }
                    ++populatedCount;
                }
                if (descriptor.dependencyCount != populatedCount) {
                    return Reject(ImportErrorCode::MalformedData,
                                  "material dependencyCount does not match its populated slot count");
                }
            }
            break;
        }
        case ChunkTopology::ImportStatus: {
            if (descriptor.vertexCount || descriptor.indexCount || descriptor.vertexLayoutId || descriptor.dependencyCount
                || descriptor.byteSize != sizeof(model_core::ImportStatusPayload))
                return Reject(ImportErrorCode::ImportProtocolViolation,"malformed import status");
            for (auto id : descriptor.dependencyIds) if (id) return Reject(ImportErrorCode::ImportProtocolViolation,"status dependency");
            model_core::ImportStatusPayload status{};
            std::memcpy(&status,section.data()+descriptor.normalizedRangeOffset,sizeof(status));
            if ((status.flags & ~model_core::kStatusKnownMask) || status.optionalFeatureWarnings > 64
                || status.textureWarnings > 64 || status.reserved)
                return Reject(ImportErrorCode::ImportProtocolViolation,"unknown or over-limit import status");
            break;
        }
        case ChunkTopology::TextureWarning: {
            if (descriptor.vertexCount || descriptor.indexCount || descriptor.vertexLayoutId || descriptor.dependencyCount
                || descriptor.byteSize!=sizeof(uint32_t)) return Reject(ImportErrorCode::MalformedData,"malformed texture warning");
            for (auto id : descriptor.dependencyIds) if (id) return Reject(ImportErrorCode::MalformedData,"warning dependency");
            uint32_t count=0; std::memcpy(&count,section.data()+descriptor.normalizedRangeOffset,sizeof(count));
            if (!count || count>64) return Reject(ImportErrorCode::MalformedData,"texture warning count out of range");
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
            if (imageHeader.width>model_core::kMaxTextureDimension || imageHeader.height>model_core::kMaxTextureDimension)
                return Reject(ImportErrorCode::ResourceLimit,"image dimension cap exceeded");
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

            // Aggregate decoded-pixel budget (all mips), re-enforced here
            // independent of the worker's own accounting.
            auto rgbaBytes=ComputeImagePixelBytes(PixelFormatId::RGBA8_UNORM,imageHeader.width,imageHeader.height,imageHeader.mipLevels);
            auto pixelCount=rgbaBytes ? std::optional<uint64_t>(*rgbaBytes/4) : std::nullopt;
            auto newAggregate = pixelCount ? CheckedAdd(aggregateImagePixels, *pixelCount) : std::nullopt;
            if (!pixelCount || !newAggregate) {
                return Reject(ImportErrorCode::ResourceLimit, "aggregate decoded texture pixel count overflows");
            }
            aggregateImagePixels = *newAggregate;
            auto newBytes=CheckedAdd(aggregateImageBytes,imageHeader.pixelDataByteSize);
            if (!newBytes || *newBytes>model_core::kMaxAggregateTextureBytes
                || aggregateImagePixels>model_core::kMaxAggregateTexturePixels)
                return Reject(ImportErrorCode::ResourceLimit,"generation texture expansion cap exceeded");
            aggregateImageBytes=*newBytes;
            if (imageHeader.reserved0) {
                auto parent=images.find(imageHeader.reserved0);
                if (parent==images.end() || parent->second.reserved0 || imageHeader.reserved0==descriptor.chunkId)
                    return Reject(ImportErrorCode::MalformedData,"image refinement root is not an earlier initial image");
                const auto& old=parent->second;
                uint32_t w=imageHeader.width,h=imageHeader.height,levels=imageHeader.mipLevels;
                while ((w>old.width || h>old.height) && levels>1) { w=std::max(1u,w/2);h=std::max(1u,h/2);--levels; }
                if (imageHeader.pixelFormat!=old.pixelFormat || imageHeader.colorSpace!=old.colorSpace
                    || (imageHeader.width==old.width && imageHeader.height==old.height)
                    || w!=old.width || h!=old.height || levels!=old.mipLevels)
                    return Reject(ImportErrorCode::MalformedData,"non-monotonic or incompatible image refinement");
                auto latest=imageHeader; latest.reserved0=0; parent->second=latest;
            }
            images.emplace(descriptor.chunkId,imageHeader);
            break;
        }
        default:
            return Reject(ImportErrorCode::MalformedData, "unrecognized topology");
        }

        // 12h. Recompute the per-chunk checksum over the validated payload
        // range. Common to every topology.
        auto chunkPayloadView
            = section.subspan(descriptor.normalizedRangeOffset, descriptor.byteSize);
        uint64_t recomputedChunkChecksum = WireChecksum64(chunkPayloadView);
        if (descriptor.lodLevel != model_core::kScanLod
            && recomputedChunkChecksum != descriptor.chunkChecksum) {
            return Reject(ImportErrorCode::MalformedData, "chunk checksum mismatch");
        }

        // 12i. Only now, copy the payload into host-owned private memory.
        std::vector<std::byte> payload(chunkPayloadView.begin(), chunkPayloadView.end());
        result.chunks.push_back(ValidatedChunk{ descriptor, std::move(payload), header.scene });
    }

    // Resolve the current batch's hierarchy only after every fixed record has
    // been copied and validated. Earlier-batch parents are immutable catalog
    // entries, while current-batch forward parent references are handled by
    // this bounded DFS. A cross-batch edge can only point backwards, so a
    // cycle cannot be hidden across reused section windows.
    std::unordered_map<uint32_t, uint8_t> nodeVisit;
    std::function<bool(uint32_t)> resolveNode = [&](uint32_t id) -> bool {
        if (scenes.nodes.contains(id))
            return true;
        const auto pending = pendingNodes.find(id);
        if (pending == pendingNodes.end())
            return false;
        uint8_t& state = nodeVisit[id];
        if (state == 1)
            return false;
        if (state == 2)
            return true;
        state = 1;
        KnownNodeRecord record{};
        record.payload = pending->second;
        record.depth = 1;
        record.visible = (record.payload.flags & model_core::kSceneRecordVisible) != 0;
        std::memcpy(record.worldTransform, record.payload.localTransform, sizeof(record.worldTransform));
        if (record.payload.parentNodeId) {
            if (!resolveNode(record.payload.parentNodeId))
                return false;
            const auto& parent = scenes.nodes.at(record.payload.parentNodeId);
            if (parent.depth >= model_core::kMaxSceneHierarchyDepth
                || !MultiplyAffine(record.payload.localTransform, parent.worldTransform,
                                   record.worldTransform))
                return false;
            record.depth = parent.depth + 1;
            record.visible = record.visible && parent.visible;
        }
        scenes.nodes.emplace(id, record);
        state = 2;
        return true;
    };
    for (const auto& [id, node] : pendingNodes) {
        (void)node;
        if (!resolveNode(id))
            return Reject(ImportErrorCode::MalformedData, "cyclic, unresolved, or over-depth node hierarchy");
    }
    if (scenes.nodes.size() > objectLimit || scenes.nodes.size() > header.scene.nodeCount)
        return Reject(ImportErrorCode::ResourceLimit, "node catalog exceeds declared or product limit");

    for (const auto& [id, instance] : pendingInstances) {
        const auto geometry = scenes.geometry.find(instance.geometryChunkId);
        const auto node = scenes.nodes.find(instance.nodeId);
        if (geometry == scenes.geometry.end() || node == scenes.nodes.end()
            || geometry->second.lodLevel == model_core::kScanLod)
            return Reject(ImportErrorCode::MalformedData, "instance reference is unresolved");
        double expectedMin[3]{}, expectedMax[3]{};
        if (!TransformGeometryBounds(geometry->second, node->second.worldTransform,
                                     expectedMin, expectedMax))
            return Reject(ImportErrorCode::MalformedData, "instance transform produces invalid bounds");
        for (uint32_t axis = 0; axis < 3; ++axis) {
            if (!BoundsEqual(instance.worldMin[axis], expectedMin[axis])
                || !BoundsEqual(instance.worldMax[axis], expectedMax[axis]))
                return Reject(ImportErrorCode::MalformedData, "instance bounds do not match geometry and transform");
        }
        scenes.instances.emplace(id, instance);
    }
    if (scenes.instances.size() > objectLimit)
        return Reject(ImportErrorCode::ResourceLimit, "instance catalog exceeds product limit");

    // 13. Aggregate decoded-texture-pixel budget (1 gigapixel, Tier A),
    // enforced across the whole batch after every chunk's own checks pass.
    if (aggregateImagePixels > kMaxAggregateDecodedTexturePixels) {
        return Reject(ImportErrorCode::ResourceLimit, "aggregate decoded texture pixel budget exceeded");
    }

    // 14. Every chunk passed.
    result.ok = true;
    result.errorCode = ImportErrorCode::None;
    if (sceneCatalog)
        *sceneCatalog = std::move(scenes);
    return result;
}

} // namespace import_broker
