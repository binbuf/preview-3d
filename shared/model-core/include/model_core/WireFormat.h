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

namespace model_core {

constexpr uint32_t kSectionMagic = 0x50334457; // "P3DW"
constexpr uint32_t kCurrentProtocolVersion = 1;
constexpr uint32_t kMaxDependencyIds = 4;

enum class ChunkTopology : uint32_t {
    Unknown = 0,
    TriangleList = 1,
    PointList = 2,
    Material = 3, // payload is a model_core::MaterialPayload (MaterialPayload.h)
    Image = 4,    // payload is a model_core::ImagePayloadHeader + pixel bytes (PixelFormats.h)
};

#pragma pack(push, 1)

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
};
static_assert(sizeof(SectionHeader) == 40, "SectionHeader wire layout changed");

// Fixed-width, one per chunk, packed contiguously starting at offset
// sizeof(SectionHeader). Never variable-length or self-describing -- the
// host must be able to bounds-check it before interpreting any of it.
struct ChunkDescriptor {
    uint64_t sourceRangeOffset;     // opaque provenance value (stands in for a real source-file offset)
    uint64_t sourceRangeLength;
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
    // Image -- must have dependencyCount==0 (images reference nothing),
    // which makes the mesh -> material -> image graph acyclic by
    // construction.
    uint32_t dependencyIds[kMaxDependencyIds];
    uint32_t dependencyCount;        // how many of dependencyIds[] are populated, <= kMaxDependencyIds
    uint64_t chunkChecksum;          // FNV-1a64 over payload bytes [normalizedRangeOffset, +byteSize)
};
static_assert(sizeof(ChunkDescriptor) == 92, "ChunkDescriptor wire layout changed");

#pragma pack(pop)

constexpr size_t kSectionHeaderSize = sizeof(SectionHeader);
constexpr size_t kChunkDescriptorSize = sizeof(ChunkDescriptor);

} // namespace model_core
