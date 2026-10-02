#pragma once

// T21 STL family adapter (ADR-0004: product parser, no third-party library).
//
// Renders ASCII and binary `.stl` through the frozen `IFamilyAdapter`
// lifecycle (FamilyAdapter.h). It is selected only by the routed
// `Family::Stl` CLSID -- never by content or extension (FamilyRouting.h) -- and
// then detects the ASCII/binary dialect on the stream itself:
//
//   * a binary-shaped file (declared facet count and length consistent with the
//     84-byte prefix + 50-byte facets) is always read as binary, even when its
//     free-form header starts with "solid";
//   * otherwise a leading "solid" selects the bounded ASCII tokenizer.
//
// Both dialects reuse `shared/parser-core` (T07): the checked binary header /
// range arithmetic (`StlParserCore`) and the per-facet normal policy
// (`NormalizeStlFacet`: supplied normal when credible, generated flat normal
// otherwise; non-finite and degenerate facets dropped). This translation unit
// stays PCH-free and free of GDI/COM so `Tests.Unit.exe` and
// `Tests.ProviderHost.exe` compile the exact source the DLL links.
//
// All reads are bounded and poll the per-call deadline at facet-block units;
// the declared facet count is validated against `kMaxStlFacets` and the actual
// source length before any facet buffer is allocated, so an adversarial count
// cannot force an unbounded read or allocation. A false return from the
// geometry sink is the sampler's cap stop, not a failure.

#include "FamilyAdapter.h"
#include "ProviderTypes.h"

#include "parser_core/StlParserCore.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace preview3d::provider {

class StlAdapter final : public IFamilyAdapter {
public:
    StlAdapter() = default;
    ~StlAdapter() noexcept override = default;

    StlAdapter(const StlAdapter&) = delete;
    StlAdapter& operator=(const StlAdapter&) = delete;

    ErrorCode Initialize(const AdapterInput& input) noexcept override;
    ErrorCode Parse() noexcept override;
    ErrorCode EnumerateMaterials(IMaterialSink& sink) noexcept override;
    ErrorCode EnumerateGeometry(IGeometrySink& sink) noexcept override;
    void Reset() noexcept override;

private:
    enum class Dialect : std::uint8_t { Unknown = 0, Ascii = 1, Binary = 2 };

    // Per-stage bodies, run under the T06 stage-containment shim
    // (ContainmentStage.h). They are non-noexcept so a `std::bad_alloc` from
    // product-owned allocation unwinds into the boundary instead of terminating
    // at the frozen `noexcept` public method.
    ErrorCode InitializeImpl(const AdapterInput& input);
    ErrorCode ParseImpl();
    ErrorCode EnumerateMaterialsImpl(IMaterialSink& sink);
    ErrorCode EnumerateGeometryImpl(IGeometrySink& sink);

    // Copies the bounded 84-byte prefix into `prefix_` (from the contiguous
    // view when available, otherwise a checked ReadAt) and classifies the
    // dialect. Fails closed on a short/unknown source.
    ErrorCode ClassifyDialect() noexcept;

    // Validates the binary header against kMaxStlFacets and the source extent
    // before any facet storage exists. Fills `header_`.
    ErrorCode ValidateBinaryHeader() noexcept;

    ErrorCode EmitBinary(IGeometrySink& sink);
    ErrorCode EmitAscii(IGeometrySink& sink);

    // Normalizes one facet and, when it survives, emits it. Returns false when
    // the sink asks the adapter to stop (cap reached).
    bool EmitFacet(const parser_core::Vec3& normal, const parser_core::Vec3& v0,
                   const parser_core::Vec3& v1, const parser_core::Vec3& v2,
                   IGeometrySink& sink) noexcept;

    ErrorCode SourceReadFailure() const noexcept;

    AdapterInput input_{};
    Dialect dialect_ = Dialect::Unknown;
    bool parsed_ = false;

    std::span<const std::byte> contiguous_{};
    std::uint64_t sourceSize_ = 0;
    std::byte prefix_[parser_core::kStlPrefixBytes]{};
    std::size_t prefixBytes_ = 0;
    parser_core::StlBinaryHeader header_{};
};

} // namespace preview3d::provider