#pragma once

// Versioned wire format for normalized chunk data crossing the process
// boundary between the trusted host and an import process, over a bounded
// shared-memory section. See .docs/design/03-file-formats-and-ingestion.md
// ("Wire format between the trusted process and an import process"). This
// header is consumed by both sides -- the worker process itself reads/writes
// these exact layouts -- so it lives in shared/model-core, not
// shared/import-broker (host-side only).
//
// A section is never trusted before the host has read and bounds-checked
// SectionHeader first; see shared/import-broker/SharedSectionValidator.h for
// the copy-then-validate acceptance path that enforces this.

#include <cstdint>
#include <cstddef>

namespace model_core {

constexpr uint32_t kSectionMagic = 0x50334457; // "P3DW"
constexpr uint32_t kCurrentProtocolVersion = 6;
// Product coarse/full delivery has four closed geometry roles. Scan payloads
// cross the validator, but are never allocated on the GPU or retained by it.
constexpr uint32_t kFineLod = 0, kCoarseLod = 1, kScanLod = 2, kPreviewLod = 3;
constexpr uint32_t kScanIdentity = 0x10000000u, kCoarseIdentity = 0x20000000u;
constexpr uint32_t kPreviewIdentity = 0x30000000u;
constexpr uint64_t kPreviewPrimitiveLimit = 4096, kPreviewByteLimit = 1024*1024;
constexpr uint64_t kPreviewReservedBytes = 8ull*1024*1024;
constexpr uint64_t kCoarseReservedBytes = 64ull * 1024 * 1024;
constexpr uint64_t kCoarsePrimitiveLimit = 2000000;
inline uint64_t CoarsePrimitiveCap(uint64_t valid, uint64_t nonemptyRegions = 1) {
    const uint64_t density=valid < 20 ? valid : (valid/20 > nonemptyRegions ? valid/20 : nonemptyRegions);
    return density < kCoarsePrimitiveLimit ? density : kCoarsePrimitiveLimit;
}
constexpr uint32_t kMaxDependencyIds = 4;

enum class ChunkTopology : uint32_t {
    Unknown = 0,
    TriangleList = 1,
    PointList = 2,
    Material = 3, // payload is a model_core::MaterialPayload (MaterialPayload.h)
    Image = 4,    // payload is a model_core::ImagePayloadHeader + pixel bytes (PixelFormats.h)
    TextureWarning = 5, // one uint32 fallback count, [1,64]; no paths or arbitrary worker text
    ImportStatus = 6,
    CoarseComplete = 7, // only after every validated scan region has a usable sample
};

#pragma pack(push, 1)

struct CoarseCompletePayload {
    uint32_t regions;
    uint32_t reserved;
    uint64_t primitives;
    uint64_t geometryBytes;
};
static_assert(sizeof(CoarseCompletePayload) == 24);

constexpr uint32_t kStatusProvisional = 1;
constexpr uint32_t kStatusRefining = 2;
constexpr uint32_t kStatusPressure = 4;
constexpr uint32_t kStatusKnownMask = 7;
// Closed, bounded facts. UI strings are owned by the host, never the worker.
struct ImportStatusPayload {
    uint32_t flags;
    uint32_t optionalFeatureWarnings; // saturated [0,64]
    uint32_t textureWarnings; // saturated [0,64]
    uint32_t reserved; // zero
};
static_assert(sizeof(ImportStatusPayload) == 16);

enum class SourceFormatId : uint32_t { Unknown = 0, Gltf = 1, Stl = 2, Ply = 3, Glb = 4 };
enum class UpAxisId : uint32_t { Unknown = 0, Y = 1, Z = 2 };
enum class BoundsState : uint32_t { Unknown = 0, Provisional = 1, Verified = 2 };
constexpr uint32_t kGeometryHasUv0 = 1;
constexpr uint32_t kGeometryHasColors = 2;
constexpr uint32_t kGeometryHasUv1 = 4;
constexpr uint32_t kGeometryFlagsKnownMask = kGeometryHasUv0 | kGeometryHasColors | kGeometryHasUv1;

// Generation-wide source facts, repeated unchanged in each batch. Zero units
// means unspecified; STL/PLY must never be presented as metres by assumption.
// Geometry counts and scene bounds are reduced from accepted descriptors,
// rather than trusting accessor extrema or a worker's declared scene totals.
struct SceneMetadata {
    uint64_t generationId;
    SourceFormatId format;
    UpAxisId upAxis;
    double metersPerUnit;
    uint32_t meshCount;
    uint32_t nodeCount;
    uint32_t animationCount;
    uint32_t skinCount;
    uint32_t boneCount;
    uint32_t reserved;
};
static_assert(sizeof(SceneMetadata) == 48);

// Fixed-width section header, always at offset 0 of the shared section.
// Read first and fully bounds-checked before any other field in the section
// is trusted.
struct SectionHeader {
    uint32_t magic;           // must equal kSectionMagic
    uint32_t protocolVersion; // must equal kCurrentProtocolVersion; unrecognized -> reject outright
    uint64_t generationId;    // staleness guard
    uint64_t sectionLength;   // total meaningful bytes (header+descriptors+payload); must be
                               // verified <= the actual mapped view size before being trusted
                               // for anything else
    uint32_t chunkCount;      // number of ChunkDescriptor entries immediately following the header
    uint32_t reserved;        // must be 0
    uint64_t sectionChecksum; // FNV-1a64 over bytes [sizeof(SectionHeader), sectionLength) --
                               // the descriptor table plus all payload bytes, not the header itself
    SceneMetadata scene;
};
static_assert(sizeof(SectionHeader) == 88, "SectionHeader wire layout changed");

// Fixed-width, one per chunk, packed contiguously starting at offset
// sizeof(SectionHeader). Never variable-length or self-describing -- the
// host must be able to bounds-check it before interpreting any of it.
struct ChunkDescriptor {
    uint64_t
        sourceRangeOffset; // STL/PLY: byte offset. glTF: (mesh primitive ordinal << 32) | first index element
    uint64_t sourceRangeLength; // STL/PLY: byte extent. glTF: index-element count; meshId/nodeId disambiguate
    uint64_t normalizedRangeOffset; // byte offset of this chunk's payload, relative to section start
    uint64_t normalizedRangeLength; // redundant cross-check against byteSize
    ChunkTopology topology;
    uint32_t indexCount;
    uint32_t vertexCount;
    uint32_t vertexLayoutId;        // numeric ID from the closed VertexLayoutId enumeration --
                                      // never a raw stride/format the receiver must interpret unchecked
    uint32_t lodLevel;
    uint32_t chunkId;                // this chunk's own identity, referenced by other chunks' dependencyIds
    uint64_t byteSize;               // declared payload byte size
    // 0 = unused slot; fixed-width, no variable list.
    // TriangleList/PointList -- a generic "derived from/associated with"
    // reference to other chunks (e.g. a proxy/LOD point cloud's relationship
    // to the mesh it was derived from, or a mesh's relationship to its
    // Material chunk); the validator checks only that each populated id
    // resolves to an existing chunk, not one fixed target topology -- a
    // consumer determines a given slot's meaning by inspecting the target
    // chunk's own topology.
    // Material -- dependencyIds[0..3] are up to 4 Image chunk ids in fixed
    // slot order {baseColor, metallicRoughness, normal, emissive}; each must
    // resolve to a chunk with Image topology specifically (a new invariant,
    // since nothing wrote Material chunks before this).
    // Image -- must have dependencyCount==0. Refinement roots are encoded
    // separately in ImagePayloadHeader and must identify an earlier initial
    // image. The mesh -> material -> image dependency graph stays acyclic.
    uint32_t dependencyIds[kMaxDependencyIds];
    uint32_t dependencyCount;        // how many of dependencyIds[] are populated, <= kMaxDependencyIds
    uint64_t chunkChecksum;          // FNV-1a64 over payload bytes [normalizedRangeOffset, +byteSize)
    double origin[3];                // native-space cluster origin; positions are LOCAL floats
    float localMin[3];
    float localMax[3];               // exact extrema of finite normalized positions
    uint32_t meshId;                 // source mesh/node identity (0 = unspecified)
    uint32_t nodeId;
    uint32_t geometryFlags;
    BoundsState boundsState;         // geometry must be Verified; other topologies Unknown
    uint32_t sourceElementOffset;    // PLY triangle fan offset in first source face, otherwise zero
};
static_assert(sizeof(ChunkDescriptor) == 160, "ChunkDescriptor wire layout changed");

#pragma pack(pop)

constexpr size_t kSectionHeaderSize = sizeof(SectionHeader);
constexpr size_t kChunkDescriptorSize = sizeof(ChunkDescriptor);

} // namespace model_core
