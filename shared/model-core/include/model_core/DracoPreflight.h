#pragma once

// Shared Draco bitstream preflight (SEC-02).
//
// `draco::Decoder::DecodeMeshFromBuffer` allocates mesh connectivity from
// counts the compressed bitstream itself declares -- the edgebreaker path
// resets a CornerTable sized by the declared face count *before* it verifies
// that the stream actually contains that many symbols. A tiny hostile
// bitstream can therefore drive a multi-gigabyte working set from a handful of
// bytes, and the import worker / thumbnail provider only checked the *glTF
// accessor* counts after the decoder returned.
//
// This header parses the same fixed header, optional metadata block and
// connectivity count preamble the pinned decoder reads, using Draco's own
// public primitives (PointCloudDecoder::DecodeHeader, MetadataDecoder,
// DecodeVarint) so the field layout is never forked. ValidateDracoCounts then
// rejects a stream whose declared face count does not match the caller's glTF
// accessors, or whose declared working set exceeds the product budget, before
// any third-party allocation happens.
//
// Cancellation: Draco exposes no progress/cancel callback, so a decode that
// passes this preflight still runs to completion uninterruptibly. The product
// bounds that window with the decoded working-set/triangle caps below and the
// compressed-span cap the adapters already apply; see ADR 0030.

#include <draco/compression/config/compression_shared.h>
#include <draco/compression/point_cloud/point_cloud_decoder.h>
#include <draco/core/decoder_buffer.h>
#include <draco/core/macros.h>
#include <draco/core/varint_decoding.h>
#include <draco/metadata/geometry_metadata.h>
#include <draco/metadata/metadata_decoder.h>

#include "platform/CheckedMath.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace model_core {

// Test-only evidence seam: incremented immediately before the third-party
// Draco decoder is invoked, so a regression test can prove a hostile stream was
// rejected before any library allocation. A relaxed increment on decode entry
// is negligible in production.
inline std::atomic<std::uint64_t>& DracoDecoderInvocations() noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}

// Declared connectivity counts pulled from the bitstream preamble.
struct DracoDeclaredCounts {
    // Triangle count exactly as the decoder will materialize it.
    std::uint32_t faces = 0;
    // Upper bound on decoded points: num_encoded_vertices + split symbols for
    // edgebreaker (attribute dedup only reduces it), or num_points exactly for
    // the sequential method.
    std::uint64_t pointsUpperBound = 0;
    // True when pointsUpperBound is the exact glTF-facing point count (the
    // sequential encoder stores num_points directly).
    bool pointsExact = false;
};

enum class DracoPreflightStatus {
    Ok,
    // The bitstream is not a supported triangular-mesh Draco stream, is
    // truncated, or declares counts inconsistent with the glTF accessors.
    Malformed,
    // The declared working set or triangle count exceeds the product budget.
    OverLimit,
};

// Parses the fixed header, skips any metadata block, and reads the connectivity
// count preamble for edgebreaker/sequential meshes. Returns nullopt for
// anything the pinned decoder itself would reject (unsupported geometry type,
// method, version, or a truncated preamble) -- callers treat that as
// MalformedData without invoking the decoder.
inline std::optional<DracoDeclaredCounts> ParseDracoDeclaredCounts(
    std::span<const std::byte> compressed) noexcept
{
    draco::DecoderBuffer buffer;
    buffer.Init(reinterpret_cast<const char*>(compressed.data()), compressed.size());

    draco::DracoHeader header;
    if (!draco::PointCloudDecoder::DecodeHeader(&buffer, &header).ok()) {
        return std::nullopt;
    }
    if (header.encoder_type != draco::TRIANGULAR_MESH) {
        return std::nullopt;
    }
    if (header.encoder_method != draco::MESH_EDGEBREAKER_ENCODING
        && header.encoder_method != draco::MESH_SEQUENTIAL_ENCODING) {
        return std::nullopt;
    }
    // The pinned decoder supports bitstream major 2 up to minor 2 (and, with
    // backwards compatibility enabled, major 1). Anything else it rejects.
    if (header.version_major > 2 || (header.version_major == 2 && header.version_minor > 2)) {
        return std::nullopt;
    }
    const std::uint16_t version
        = DRACO_BITSTREAM_VERSION(header.version_major, header.version_minor);
    buffer.set_bitstream_version(version);

    if (version >= DRACO_BITSTREAM_VERSION(1, 3) && (header.flags & METADATA_FLAG_MASK) != 0) {
        draco::GeometryMetadata metadata;
        draco::MetadataDecoder decoder;
        if (!decoder.DecodeGeometryMetadata(&buffer, &metadata)) {
            return std::nullopt;
        }
    }

    auto varint = [&buffer](std::uint32_t& out) {
        return draco::DecodeVarint<std::uint32_t>(&out, &buffer);
    };
    auto fixed = [&buffer](std::uint32_t& out) { return buffer.Decode(&out); };

    // The edgebreaker decoder's InitializeDecoder reads the traversal decoder
    // selector before the connectivity count preamble; the sequential decoder
    // has no such byte. See MeshEdgebreakerDecoder::InitializeDecoder.
    if (header.encoder_method == draco::MESH_EDGEBREAKER_ENCODING) {
        std::uint8_t traversalDecoderType = 0;
        if (!buffer.Decode(&traversalDecoderType)
            || traversalDecoderType > draco::MESH_EDGEBREAKER_VALENCE_ENCODING) {
            return std::nullopt;
        }
    }

    DracoDeclaredCounts counts;
    if (header.encoder_method == draco::MESH_SEQUENTIAL_ENCODING) {
        std::uint32_t numFaces = 0;
        std::uint32_t numPoints = 0;
        if (version < DRACO_BITSTREAM_VERSION(2, 2)) {
            if (!fixed(numFaces) || !fixed(numPoints)) {
                return std::nullopt;
            }
        } else if (!varint(numFaces) || !varint(numPoints)) {
            return std::nullopt;
        }
        counts.faces = numFaces;
        counts.pointsUpperBound = numPoints;
        counts.pointsExact = true;
        return counts;
    }

    // Edgebreaker: the decoder reads (optionally) num_new_vertices, then the
    // encoded vertex count and face count, then attribute-data and symbol
    // counts.
    if (version < DRACO_BITSTREAM_VERSION(2, 2)) {
        std::uint32_t numNewVertices = 0;
        if (version < DRACO_BITSTREAM_VERSION(2, 0)) {
            if (!fixed(numNewVertices)) {
                return std::nullopt;
            }
        } else if (!varint(numNewVertices)) {
            return std::nullopt;
        }
    }
    std::uint32_t numEncodedVertices = 0;
    std::uint32_t numFaces = 0;
    if (version < DRACO_BITSTREAM_VERSION(2, 0)) {
        if (!fixed(numEncodedVertices) || !fixed(numFaces)) {
            return std::nullopt;
        }
    } else if (!varint(numEncodedVertices) || !varint(numFaces)) {
        return std::nullopt;
    }
    std::uint8_t numAttributeData = 0;
    if (!buffer.Decode(&numAttributeData)) {
        return std::nullopt;
    }
    std::uint32_t numEncodedSymbols = 0;
    std::uint32_t numEncodedSplitSymbols = 0;
    if (version < DRACO_BITSTREAM_VERSION(2, 0)) {
        if (!fixed(numEncodedSymbols) || !fixed(numEncodedSplitSymbols)) {
            return std::nullopt;
        }
    } else if (!varint(numEncodedSymbols) || !varint(numEncodedSplitSymbols)) {
        return std::nullopt;
    }

    counts.faces = numFaces;
    // is_vert_hole_ / the point maps are sized by encoded vertices plus split
    // symbols; use the same checked sum the decoder reserves.
    counts.pointsUpperBound = static_cast<std::uint64_t>(numEncodedVertices)
        + static_cast<std::uint64_t>(numEncodedSplitSymbols);
    counts.pointsExact = false;
    return counts;
}

// Full preflight: parse the declared counts, require the declared face count to
// match the caller's glTF accessor face count exactly, and bound the declared
// working set. `outCounts` is filled even on OverLimit so callers can report
// the first exceeded declaration.
inline DracoPreflightStatus ValidateDracoCounts(
    std::span<const std::byte> compressed, std::size_t expectedVertexCount,
    std::size_t expectedIndexCount, std::uint64_t maxTriangles,
    std::uint64_t maxDecodedWorkingSetBytes,
    DracoDeclaredCounts* outCounts = nullptr) noexcept
{
    const auto parsed = ParseDracoDeclaredCounts(compressed);
    if (!parsed.has_value()) {
        return DracoPreflightStatus::Malformed;
    }
    const DracoDeclaredCounts counts = *parsed;
    if (outCounts != nullptr) {
        *outCounts = counts;
    }

    if (expectedIndexCount % 3 != 0) {
        return DracoPreflightStatus::Malformed;
    }
    const std::uint64_t expectedFaces = expectedIndexCount / 3;
    if (counts.faces != expectedFaces) {
        return DracoPreflightStatus::Malformed;
    }
    if (counts.pointsExact && counts.pointsUpperBound != expectedVertexCount) {
        return DracoPreflightStatus::Malformed;
    }
    if (counts.faces > maxTriangles) {
        return DracoPreflightStatus::OverLimit;
    }

    // Mirrors the accessor-based budget already applied by the adapters, but
    // evaluated on the decoder-declared counts. A stream that declared a huge
    // face count would already have failed the exact match above; this bounds
    // the remaining declared vertex/split allocation.
    const auto pointBytes = platform::CheckedMultiply(counts.pointsUpperBound, std::uint64_t{64});
    const auto indexBytes
        = platform::CheckedMultiply(static_cast<std::uint64_t>(counts.faces) * 3, std::uint64_t{8});
    const auto total = pointBytes && indexBytes ? platform::CheckedAdd(*pointBytes, *indexBytes)
                                                : std::nullopt;
    if (!total.has_value() || *total > maxDecodedWorkingSetBytes) {
        return DracoPreflightStatus::OverLimit;
    }
    return DracoPreflightStatus::Ok;
}

} // namespace model_core