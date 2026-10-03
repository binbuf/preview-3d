// T21 STL family adapter implementation (see StlAdapter.h).

#include "StlFamilyAdapter.h"

#include "AllocationLedger.h"
#include "ContainmentStage.h"
#include "Deadline.h"
#include "ProviderLimits.h"

#include "parser_core/AsciiTokenizer.h"
#include "parser_core/StlParserCore.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace preview3d::provider {
namespace {

// Facets normalized per bounded read. 4096 facets is 200 KiB of source, small
// enough to poll the deadline frequently and large enough to amortize the
// per-block ReadAt. Charged to the T06 ledger before allocation.
constexpr std::uint32_t kFacetsPerBlock = 4096;
constexpr std::uint64_t kBlockBytes =
    static_cast<std::uint64_t>(kFacetsPerBlock) * parser_core::kStlFacetBytes;

} // namespace

ErrorCode StlAdapter::SourceReadFailure() const noexcept
{
    // A failed bounded read is either the cooperative deadline or a short-read
    // inconsistency (BoundedSource.h). Both fail closed; only the deadline is a
    // distinct typed outcome.
    if (input_.deadline != nullptr && !input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    return ErrorCode::MalformedData;
}

void StlAdapter::Reset() noexcept
{
    input_ = AdapterInput{};
    dialect_ = Dialect::Unknown;
    parsed_ = false;
    contiguous_ = {};
    sourceSize_ = 0;
    prefixBytes_ = 0;
    header_ = parser_core::StlBinaryHeader{};
}

ErrorCode StlAdapter::Initialize(const AdapterInput& input) noexcept
{
    return RunContainedStageMember([this, &input]() { return InitializeImpl(input); },
                                   DiagnosticStage::AdapterInitialize);
}

ErrorCode StlAdapter::InitializeImpl(const AdapterInput& input)
{
    Reset();
    if (input.source == nullptr || input.limits == nullptr || input.deadline == nullptr) {
        return ErrorCode::InternalImporterFailure;
    }
    input_ = input;
    return ErrorCode::None;
}

ErrorCode StlAdapter::ClassifyDialect() noexcept
{
    // Prefer the bounded contiguous view: the T12 source materializes
    // non-seekable input up to the 128 MiB cap and returns it here. When the
    // whole source is not available as one span, read just the checked prefix
    // and rely on bounded range reads for the rest.
    contiguous_ = input_.source->ContiguousView();
    sourceSize_ = input_.source->Size();
    if (!contiguous_.empty() && sourceSize_ == 0) {
        sourceSize_ = contiguous_.size();
    }

    if (!contiguous_.empty()) {
        prefixBytes_ = (std::min)(contiguous_.size(), parser_core::kStlPrefixBytes);
        std::memcpy(prefix_, contiguous_.data(), prefixBytes_);
    } else {
        if (sourceSize_ < parser_core::kStlPrefixBytes) {
            return ErrorCode::MalformedData;
        }
        if (!input_.source->ReadAt(
                0, std::span<std::byte>(prefix_, parser_core::kStlPrefixBytes))) {
            return SourceReadFailure();
        }
        prefixBytes_ = parser_core::kStlPrefixBytes;
    }

    const std::span<const std::byte> prefix(prefix_, prefixBytes_);
    dialect_ = parser_core::IsAsciiStl(prefix, sourceSize_) ? Dialect::Ascii : Dialect::Binary;

    if (dialect_ == Dialect::Ascii) {
        // The ASCII tokenizer needs the whole source as one bounded span; the
        // frozen 128 MiB contiguous-backing path is the ceiling, beyond which
        // this dialect fails closed to the generic icon rather than allocating
        // an unbounded buffer.
        if (sourceSize_ > ProviderLimits::kContiguousBackingMaxBytes) {
            return ErrorCode::ResourceLimit;
        }
        parsed_ = true;
        return ErrorCode::None;
    }
    return ValidateBinaryHeader();
}

ErrorCode StlAdapter::ValidateBinaryHeader() noexcept
{
    if (prefixBytes_ < parser_core::kStlPrefixBytes) {
        return ErrorCode::MalformedData;
    }

    // The declared facet count is inspected before any count-derived
    // multiplication or allocation. One past the Tier A facet cap is a resource
    // limit; a structurally consistent-but-short file is malformed data.
    const std::uint32_t triangleCount =
        parser_core::ReadU32LE(prefix_ + parser_core::kStlHeaderBytes);
    if (triangleCount > parser_core::kMaxStlFacets) {
        return ErrorCode::ResourceLimit;
    }

    auto facetsBytes = CheckedMultiply(triangleCount, parser_core::kStlFacetBytes);
    auto expectedMinSize = facetsBytes
        ? CheckedAdd(parser_core::kStlPrefixBytes, *facetsBytes)
        : std::nullopt;
    if (!expectedMinSize.has_value()) {
        return ErrorCode::ResourceLimit;
    }
    // Trailing bytes past the declared facet table are tolerated; only a
    // truncated (too-short) source is rejected.
    if (sourceSize_ < *expectedMinSize) {
        return ErrorCode::MalformedData;
    }

    header_.triangleCount = triangleCount;
    header_.expectedMinSize = *expectedMinSize;
    parsed_ = true;
    return ErrorCode::None;
}

ErrorCode StlAdapter::Parse() noexcept
{
    return RunContainedStageMember([this]() { return ParseImpl(); }, DiagnosticStage::Parse);
}

ErrorCode StlAdapter::ParseImpl()
{
    if (!input_.deadline->Checkpoint()) {
        return ErrorCode::Cancelled;
    }
    dialect_ = Dialect::Unknown;
    parsed_ = false;
    return ClassifyDialect();
}

ErrorCode StlAdapter::EnumerateMaterials(IMaterialSink& sink) noexcept
{
    return RunContainedStageMember([this, &sink]() { return EnumerateMaterialsImpl(sink); },
                                   DiagnosticStage::Materials);
}

ErrorCode StlAdapter::EnumerateMaterialsImpl(IMaterialSink& sink)
{
    // STL carries no material dialect. The neutral fallback is emitted as
    // material 1 so every triangle resolves through the 1-based table
    // (ProviderTypes.h, "material fallbacks").
    sink.OnMaterial(1, NeutralMaterial());
    return ErrorCode::None;
}

bool StlAdapter::EmitFacet(const parser_core::Vec3& normal, const parser_core::Vec3& v0,
                           const parser_core::Vec3& v1, const parser_core::Vec3& v2,
                           IGeometrySink& sink) noexcept
{
    parser_core::NormalizedStlFacet facet;
    if (!parser_core::NormalizeStlFacet(parser_core::StlFacet{normal, v0, v1, v2}, facet)) {
        return true; // non-finite or degenerate facet: dropped, not a failure
    }

    TriangleSample triangle{};
    for (int corner = 0; corner < 3; ++corner) {
        VertexSample& vertex = triangle.vertices[corner];
        vertex.position[0] = static_cast<float>(facet.position[corner].x);
        vertex.position[1] = static_cast<float>(facet.position[corner].y);
        vertex.position[2] = static_cast<float>(facet.position[corner].z);
        vertex.normal[0] = static_cast<float>(facet.normal.x);
        vertex.normal[1] = static_cast<float>(facet.normal.y);
        vertex.normal[2] = static_cast<float>(facet.normal.z);
        vertex.color[0] = 1.0f;
        vertex.color[1] = 1.0f;
        vertex.color[2] = 1.0f;
        vertex.color[3] = 1.0f;
    }
    triangle.materialIndex = 1;
    return sink.OnTriangle(triangle);
}

ErrorCode StlAdapter::EmitBinary(IGeometrySink& sink)
{
    const std::uint32_t triangleCount = header_.triangleCount;
    if (triangleCount == 0) {
        return ErrorCode::None; // no geometry: the pipeline reports bad format
    }

    std::optional<AllocationReservation> reservation;
    if (input_.ledger != nullptr) {
        reservation = input_.ledger->ReserveScoped(kBlockBytes);
        if (!reservation.has_value()) {
            return ErrorCode::ResourceLimit;
        }
    }
    std::vector<std::byte> block(static_cast<std::size_t>(kBlockBytes));

    for (std::uint32_t first = 0; first < triangleCount;) {
        if (!input_.deadline->Checkpoint()) {
            return ErrorCode::Cancelled;
        }
        const std::uint32_t count = (std::min)(kFacetsPerBlock, triangleCount - first);
        const std::uint64_t offset =
            parser_core::kStlPrefixBytes + static_cast<std::uint64_t>(first) * parser_core::kStlFacetBytes;
        const std::size_t bytes = static_cast<std::size_t>(count) * parser_core::kStlFacetBytes;

        std::span<const std::byte> facets;
        if (!contiguous_.empty()) {
            facets = contiguous_.subspan(static_cast<std::size_t>(offset), bytes);
        } else {
            if (!input_.source->ReadAt(offset, std::span<std::byte>(block.data(), bytes))) {
                return SourceReadFailure();
            }
            facets = std::span<const std::byte>(block.data(), bytes);
        }

        for (std::uint32_t i = 0; i < count; ++i) {
            const std::byte* facet = facets.data() + static_cast<std::size_t>(i) * parser_core::kStlFacetBytes;
            const parser_core::Vec3 normal{parser_core::ReadFloatLE(facet),
                                           parser_core::ReadFloatLE(facet + 4),
                                           parser_core::ReadFloatLE(facet + 8)};
            const parser_core::Vec3 v0{parser_core::ReadFloatLE(facet + 12),
                                       parser_core::ReadFloatLE(facet + 16),
                                       parser_core::ReadFloatLE(facet + 20)};
            const parser_core::Vec3 v1{parser_core::ReadFloatLE(facet + 24),
                                       parser_core::ReadFloatLE(facet + 28),
                                       parser_core::ReadFloatLE(facet + 32)};
            const parser_core::Vec3 v2{parser_core::ReadFloatLE(facet + 36),
                                       parser_core::ReadFloatLE(facet + 40),
                                       parser_core::ReadFloatLE(facet + 44)};
            if (!EmitFacet(normal, v0, v1, v2, sink)) {
                return ErrorCode::None; // sampler cap stop, not a failure
            }
        }
        first += count;
    }
    return ErrorCode::None;
}

ErrorCode StlAdapter::EmitAscii(IGeometrySink& sink)
{
    std::span<const std::byte> bytes = contiguous_;
    if (bytes.empty()) {
        bytes = input_.source->ContiguousView();
    }
    if (bytes.empty()) {
        return ErrorCode::ResourceLimit;
    }

    parser_core::AsciiTokenizer tokenizer(bytes);
    const auto expectKeyword = [&tokenizer](std::string_view keyword) noexcept {
        const std::optional<std::string_view> token = tokenizer.NextToken();
        return token.has_value() && *token == keyword;
    };

    if (!expectKeyword("solid")) {
        return ErrorCode::MalformedData;
    }
    // Skip the free-form solid name up to the first "facet"/"endsolid"; the
    // bound stops a hostile file that contains neither keyword.
    std::optional<std::string_view> next;
    for (int skipped = 0;; ++skipped) {
        next = tokenizer.NextToken();
        if (!next.has_value()) {
            return ErrorCode::MalformedData;
        }
        if (*next == "facet" || *next == "endsolid") {
            break;
        }
        if (skipped >= parser_core::kMaxSolidNameTokens) {
            return ErrorCode::MalformedData;
        }
    }

    std::uint32_t facetCount = 0;
    while (next.has_value() && *next == "facet") {
        if (!input_.deadline->Checkpoint()) {
            return ErrorCode::Cancelled;
        }
        if (facetCount >= parser_core::kMaxStlFacets) {
            return ErrorCode::ResourceLimit;
        }
        ++facetCount;

        if (!expectKeyword("normal")) {
            return ErrorCode::MalformedData;
        }
        const auto nx = tokenizer.NextNumber();
        const auto ny = tokenizer.NextNumber();
        const auto nz = tokenizer.NextNumber();
        if (!nx.has_value() || !ny.has_value() || !nz.has_value()) {
            return ErrorCode::MalformedData;
        }

        if (!expectKeyword("outer") || !expectKeyword("loop")) {
            return ErrorCode::MalformedData;
        }
        parser_core::Vec3 vertices[3];
        for (parser_core::Vec3& vertex : vertices) {
            if (!expectKeyword("vertex")) {
                return ErrorCode::MalformedData;
            }
            const auto x = tokenizer.NextNumber();
            const auto y = tokenizer.NextNumber();
            const auto z = tokenizer.NextNumber();
            if (!x.has_value() || !y.has_value() || !z.has_value()) {
                return ErrorCode::MalformedData;
            }
            vertex = parser_core::Vec3{*x, *y, *z};
        }
        if (!expectKeyword("endloop") || !expectKeyword("endfacet")) {
            return ErrorCode::MalformedData;
        }

        if (!EmitFacet(parser_core::Vec3{*nx, *ny, *nz}, vertices[0], vertices[1], vertices[2], sink)) {
            return ErrorCode::None;
        }

        next = tokenizer.NextToken();
        if (!next.has_value()) {
            return ErrorCode::MalformedData; // ran out before "endsolid"
        }
    }
    if (*next != "endsolid") {
        return ErrorCode::MalformedData;
    }
    return ErrorCode::None; // trailing solid-name tokens are ignored
}

ErrorCode StlAdapter::EnumerateGeometry(IGeometrySink& sink) noexcept
{
    return RunContainedStageMember([this, &sink]() { return EnumerateGeometryImpl(sink); },
                                   DiagnosticStage::Geometry);
}

ErrorCode StlAdapter::EnumerateGeometryImpl(IGeometrySink& sink)
{
    if (!parsed_) {
        return ErrorCode::InternalImporterFailure;
    }
    return dialect_ == Dialect::Ascii ? EmitAscii(sink) : EmitBinary(sink);
}

} // namespace preview3d::provider