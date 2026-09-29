#pragma once

// Format-agnostic STL parser primitives shared by the import worker and the
// thumbnail provider (T07, ADR-0004). This header and its .cpp hold only
// std/model-core/platform dependencies: no IPC, mapping, broker, worker or
// viewer type appears here, so the same source compiles into both
// Preview3DImportWorker.exe and Preview3DThumbnailProvider.dll.
//
// The wire-emitting loop (BoundedChunkWriter/ChunkBatchSink) and the
// MappedFile-backed Tier A streaming stay in the worker adapter; only the
// per-facet validation, supplied/flat-normal policy, binary-header shape and
// dialect detection move here, because those are the same for either consumer.

#include "model_core/TierALimits.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace parser_core {

// Binary-STL layout: 80-byte header + uint32 facet count + 50-byte facets.
inline constexpr std::size_t kStlHeaderBytes = 80;
inline constexpr std::size_t kStlCountBytes = 4;
inline constexpr std::size_t kStlFacetBytes = 50; // 12 (normal) + 36 (vertices) + 2 (unused)
inline constexpr std::size_t kStlPrefixBytes = kStlHeaderBytes + kStlCountBytes; // 84

// Tier A facet count is checked before any cluster allocation.
inline constexpr std::uint32_t kMaxStlFacets = model_core::kTierATriangleLimit;

// Bounds how many tokens after "solid" are skipped looking for the first
// "facet"/"endsolid" keyword -- the free-form solid name can be empty, one
// word, or several; this just stops a hostile file with neither keyword from
// scanning forever.
inline constexpr int kMaxSolidNameTokens = 256;

// Double-precision facet math, matching the worker's historical semantics.
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    Vec3 operator-(const Vec3& other) const { return { x - other.x, y - other.y, z - other.z }; }
};

Vec3 Cross(const Vec3& a, const Vec3& b);
double Dot(const Vec3& a, const Vec3& b);
bool IsFinite(const Vec3& v);

float ReadFloatLE(const std::byte* p);
std::uint32_t ReadU32LE(const std::byte* p);

// One raw binary/ASCII facet: supplied normal plus three vertices.
struct StlFacet {
    Vec3 normal;
    Vec3 v0;
    Vec3 v1;
    Vec3 v2;
};

// A facet that survived validation: the per-corner positions and the chosen
// unit normal. Returned false means the facet was dropped (non-finite or
// degenerate) and nothing is emitted.
struct NormalizedStlFacet {
    Vec3 position[3];
    Vec3 normal;
};

bool NormalizeStlFacet(const StlFacet& facet, NormalizedStlFacet& out);

struct StlBinaryHeader {
    std::uint32_t triangleCount = 0;
    std::uint64_t expectedMinSize = 0;
};

// True when the prefix names a binary-shaped STL: enough bytes, a declared
// count within Tier A, and a checked prefix+facets*50 size. expectedMinSize is
// always set to the checked minimum (0 when the arithmetic overflows).
bool DecodeStlBinaryHeader(std::span<const std::byte> prefix, StlBinaryHeader& out);

// A file is treated as binary whenever its declared triangle count and actual
// length are consistent with the binary layout (trailing bytes tolerated) --
// even if it also starts with "solid" -- and as ASCII only when it is not
// binary-shaped but does start with "solid".
bool IsAsciiStl(std::span<const std::byte> sourcePrefix, std::uint64_t sourceSize);

} // namespace parser_core