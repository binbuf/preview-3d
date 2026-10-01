#pragma once

// T23 PLY family adapter (ADR-0004: product parser, no third-party library).
//
// Renders ASCII and binary little/big-endian `.ply` through the frozen
// `IFamilyAdapter` lifecycle (FamilyAdapter.h). It is selected only by the
// routed `Family::Ply` CLSID -- never by content or extension -- and reads the
// header's declared `format` to pick the dialect:
//
//   * ASCII PLY tokenizes the whole bounded source (`parser_core::AsciiTokenizer`);
//   * binary PLY uses endian-aware scalar reads in bounded windows.
//
// Both dialects reuse `shared/parser-core/PlyParserCore.h` (T07): the scalar
// type model, endian-aware `ReadScalarAsDouble`, `NormalizeColor`, and the
// bounded `ParseHeader`. This translation unit stays PCH-free and free of
// GDI/COM so `Tests.Unit.exe` and `Tests.ProviderHost.exe` compile the exact
// source the DLL links.
//
// Geometry shape:
//   * a `vertex` element with finite scalar `x`/`y`/`z` is required; normals
//     (`nx/ny/nz`) and colors (`red/green/blue[/alpha]` or `r/g/b[/a]`) are
//     carried when present, otherwise white/flat defaults are used;
//   * a `face` element with a `vertex_indices`/`vertex_index` integer list makes
//     this a triangle mesh (fan-triangulated); otherwise it is a point cloud;
//   * unknown elements/properties and list-typed variables are skipped within
//     fixed bounds, never sized from a declared count;
//   * binary meshes locate the vertex element with checked arithmetic and read
//     each referenced record by `vertexStart + index * stride` -- source
//     positions are never all materialized in private memory. A binary vertex
//     element with a list-typed property cannot be randomly addressed and fails
//     closed. ASCII meshes retain a bounded, ledger-charged vertex table
//     because the tokenizer has no fixed record offsets, and reject a table
//     over the accounted-scratch cap before any allocation.
//
// All reads poll the per-call deadline at bounded record units; a false return
// from the geometry sink is the sampler's inspect-cap stop, not a failure.

#include "FamilyAdapter.h"
#include "ProviderTypes.h"

#include "parser_core/AsciiTokenizer.h"
#include "parser_core/PlyParserCore.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace preview3d::provider {

class PlyAdapter final : public IFamilyAdapter {
public:
    PlyAdapter() = default;
    ~PlyAdapter() noexcept override = default;

    PlyAdapter(const PlyAdapter&) = delete;
    PlyAdapter& operator=(const PlyAdapter&) = delete;

    ErrorCode Initialize(const AdapterInput& input) noexcept override;
    ErrorCode Parse() noexcept override;
    ErrorCode EnumerateMaterials(IMaterialSink& sink) noexcept override;
    ErrorCode EnumerateGeometry(IGeometrySink& sink) noexcept override;
    void Reset() noexcept override;

private:
    // Property indices into the vertex element, plus the fixed binary record
    // stride (0 when any vertex property is a list, so the record cannot be
    // randomly addressed).
    struct VertexSchema {
        int x = -1;
        int y = -1;
        int z = -1;
        int nx = -1;
        int ny = -1;
        int nz = -1;
        int red = -1;
        int green = -1;
        int blue = -1;
        int alpha = -1;
        bool hasColors = false;
        bool hasNormals = false;
        std::uint64_t stride = 0;
    };

    // One decoded vertex: absolute double position plus normalized attributes.
    struct DecodedVertex {
        double position[3] = {0.0, 0.0, 0.0};
        float normal[3] = {0.0f, 0.0f, 0.0f};
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };

    ErrorCode LoadHeader() noexcept;
    ErrorCode ResolveSchema() noexcept;
    ErrorCode ComputeBinaryOffsets() noexcept;

    ErrorCode EmitBinaryPoints(IGeometrySink& sink) noexcept;
    ErrorCode EmitBinaryMesh(IGeometrySink& sink) noexcept;
    ErrorCode EmitAsciiPoints(IGeometrySink& sink) noexcept;
    ErrorCode EmitAsciiMesh(IGeometrySink& sink) noexcept;

    // Bounded binary primitives: a checked range read (prefers the contiguous
    // view), a checked skip, one scalar advance, and a bounded list skip.
    bool ReadBytesAt(std::uint64_t offset, std::span<std::byte> dest) noexcept;
    bool SkipBytes(std::uint64_t& cursor, std::uint64_t bytes) const noexcept;
    bool ReadScalar(std::uint64_t& cursor, parser_core::PlyScalarType type,
                    double& value) noexcept;
    ErrorCode SkipList(std::uint64_t& cursor,
                       const parser_core::PlyProperty& property) noexcept;
    ErrorCode SkipBinaryRecord(std::uint64_t& cursor,
                               const parser_core::PlyElement& element) noexcept;

    // Decodes one fixed-stride vertex record (all properties present).
    void DecodeFixedVertex(const std::byte* record, DecodedVertex& out) const noexcept;
    // Decodes one variable-width binary vertex record from `cursor`, skipping
    // list-typed properties within bounds. Leaves a non-finite position in
    // `out` when the record's position is unusable.
    ErrorCode DecodeBinaryVertex(std::uint64_t& cursor, DecodedVertex& out) noexcept;
    // Maps one vertex property value into the decoded vertex.
    void ApplyVertexProperty(int index, const parser_core::PlyProperty& property,
                             double value, DecodedVertex& out) const noexcept;
    // Normalizes a supplied normal or clears it so the rasterizer uses the
    // triangle's flat normal.
    static void NormalizeNormal(DecodedVertex& out) noexcept;

    // ASCII primitives: one vertex record and a bounded record/list skip.
    ErrorCode ReadAsciiVertex(parser_core::AsciiTokenizer& tokenizer,
                              DecodedVertex& out) noexcept;
    ErrorCode SkipAsciiRecord(parser_core::AsciiTokenizer& tokenizer,
                              const parser_core::PlyElement& element) noexcept;
    ErrorCode SkipAsciiList(parser_core::AsciiTokenizer& tokenizer,
                            const parser_core::PlyProperty& property) noexcept;

    bool EmitPoint(const DecodedVertex& vertex, IGeometrySink& sink) noexcept;
    bool EmitTriangle(const DecodedVertex& a, const DecodedVertex& b,
                      const DecodedVertex& c, IGeometrySink& sink) noexcept;

    ErrorCode SourceReadFailure() const noexcept;

    AdapterInput input_{};
    parser_core::PlyHeader header_{};
    bool parsed_ = false;

    std::span<const std::byte> contiguous_{};
    std::uint64_t sourceSize_ = 0;
    std::uint64_t bodyOffset_ = 0;

    const parser_core::PlyElement* vertexElement_ = nullptr;
    const parser_core::PlyElement* faceElement_ = nullptr;
    std::size_t vertexElementIndex_ = 0;
    std::size_t faceElementIndex_ = 0;
    std::size_t faceListProperty_ = 0;
    bool hasFace_ = false;
    bool ascii_ = false;
    bool bigEndian_ = false;

    std::uint64_t vertexCount_ = 0;
    std::uint64_t vertexStart_ = 0;
    std::uint64_t faceStart_ = 0;
    VertexSchema schema_{};

    double origin_[3] = {0.0, 0.0, 0.0};
    bool haveOrigin_ = false;
};

} // namespace preview3d::provider