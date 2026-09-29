// T21 STL family adapter coverage.
//
// `StlAdapter.cpp` and `FamilyAdapterRegistry.cpp` are compiled into
// Tests.Unit.exe, so these cases exercise the real adapter contract directly
// (Initialize -> Parse -> EnumerateMaterials -> EnumerateGeometry) and through
// the real routed pipeline (`RunThumbnailPipeline` + `DefaultThumbnailDependencies`,
// i.e. the shipped T14 sampler and T15 rasterizer). The malformed inputs the
// task calls out are asserted as typed failures, and the declared facet count is
// shown to be rejected before any facet storage exists.
//
// Fixtures are built in-memory (no committed model file) the same deterministic
// way `tests/import-isolation/StlImportTests.cpp` builds its binary/ASCII
// sources, so the provider parser is checked against the same 84-byte prefix +
// 50-byte facet layout the worker uses.

#include <catch2/catch_test_macros.hpp>

#include "AllocationLedger.h"
#include "Deadline.h"
#include "FamilyAdapter.h"
#include "FamilyAdapterRegistry.h"
#include "FamilyRouting.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"
#include "StlFamilyAdapter.h"
#include "ThumbnailPipeline.h"

#include "parser_core/StlParserCore.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace preview3d::provider;

namespace {

// A real-byte BoundedSource. `exposeContiguous == false` returns an empty
// ContiguousView so the adapter's bounded range-read path is exercised.
class ByteSource final : public BoundedSource {
public:
    explicit ByteSource(std::vector<std::byte> bytes, bool exposeContiguous = true)
        : bytes_(std::move(bytes)), exposeContiguous_(exposeContiguous)
    {
    }

    std::uint64_t Size() const noexcept override { return bytes_.size(); }
    bool Seekable() const noexcept override { return true; }

    bool ReadAt(std::uint64_t offset, std::span<std::byte> dest) override
    {
        if (offset > bytes_.size() || dest.size() > bytes_.size() - offset) {
            return false; // short read / out of range
        }
        if (!dest.empty()) {
            std::memcpy(dest.data(), bytes_.data() + offset, dest.size());
        }
        return true;
    }

    std::span<const std::byte> ContiguousView() override
    {
        return exposeContiguous_ ? std::span<const std::byte>(bytes_.data(), bytes_.size())
                                 : std::span<const std::byte>{};
    }

private:
    std::vector<std::byte> bytes_;
    bool exposeContiguous_;
};

struct CollectSink final : IGeometrySink {
    std::vector<TriangleSample> triangles;
    std::vector<PointSample> points;
    int triangleLimit = -1; // -1 = unlimited; otherwise a cap-stop request

    bool OnTriangle(const TriangleSample& triangle) noexcept override
    {
        if (triangleLimit >= 0 && static_cast<int>(triangles.size()) >= triangleLimit) {
            return false;
        }
        triangles.push_back(triangle);
        return true;
    }

    bool OnPoint(const PointSample& point) noexcept override
    {
        points.push_back(point);
        return true;
    }
};

struct CollectMaterials final : IMaterialSink {
    std::vector<std::uint32_t> indices;
    std::vector<model_core::MaterialPayload> materials;

    bool OnMaterial(std::uint32_t materialIndex,
                    const model_core::MaterialPayload& material) noexcept override
    {
        indices.push_back(materialIndex);
        materials.push_back(material);
        return true;
    }
};

struct AdapterResult {
    ErrorCode initialize = ErrorCode::None;
    ErrorCode parse = ErrorCode::None;
    ErrorCode materials = ErrorCode::None;
    ErrorCode geometry = ErrorCode::None;
    bool geometryRan = false;
    std::vector<std::uint32_t> materialIndices;
};

AdapterResult RunAdapter(StlAdapter& adapter, BoundedSource& source, Deadline& deadline,
                         AllocationLedger& ledger, IGeometrySink& sink)
{
    AdapterInput input{};
    input.source = &source;
    input.limits = &ProviderLimits::Default();
    input.deadline = &deadline;
    input.ledger = &ledger;
    input.family = Family::Stl;

    AdapterResult result;
    result.initialize = adapter.Initialize(input);
    if (result.initialize != ErrorCode::None) {
        return result;
    }
    result.parse = adapter.Parse();
    if (result.parse != ErrorCode::None) {
        return result;
    }
    CollectMaterials materials;
    result.materials = adapter.EnumerateMaterials(materials);
    result.materialIndices = materials.indices;
    result.geometryRan = true;
    result.geometry = adapter.EnumerateGeometry(sink);
    return result;
}

// One binary-STL facet: supplied normal + 3 vertices (50 bytes on disk).
struct RawFacet {
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    float v0x = 0.0f, v0y = 0.0f, v0z = 0.0f;
    float v1x = 0.0f, v1y = 0.0f, v1z = 0.0f;
    float v2x = 0.0f, v2y = 0.0f, v2z = 0.0f;
};

std::vector<std::byte> BuildBinaryStl(const std::vector<RawFacet>& facets,
                                      std::uint32_t declaredCountOverride = 0xFFFFFFFFu)
{
    std::vector<std::byte> bytes(
        80 + 4 + facets.size() * parser_core::kStlFacetBytes, std::byte{0});

    const std::uint32_t count = declaredCountOverride == 0xFFFFFFFFu
        ? static_cast<std::uint32_t>(facets.size())
        : declaredCountOverride;
    std::memcpy(bytes.data() + 80, &count, sizeof(count));

    std::size_t offset = 84;
    for (const RawFacet& facet : facets) {
        const float values[12] = {facet.nx,  facet.ny,  facet.nz,  facet.v0x, facet.v0y,
                                  facet.v0z, facet.v1x, facet.v1y, facet.v1z, facet.v2x,
                                  facet.v2y, facet.v2z};
        std::memcpy(bytes.data() + offset, values, sizeof(values));
        offset += sizeof(values);
        const std::uint16_t attributeByteCount = 0;
        std::memcpy(bytes.data() + offset, &attributeByteCount, sizeof(attributeByteCount));
        offset += sizeof(attributeByteCount);
    }
    return bytes;
}

std::vector<std::byte> StringToBytes(const std::string& text)
{
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

std::vector<std::byte> BuildAsciiStl(const std::vector<RawFacet>& facets)
{
    std::ostringstream out;
    out << "solid t\n";
    for (const RawFacet& facet : facets) {
        out << "facet normal " << facet.nx << ' ' << facet.ny << ' ' << facet.nz << "\n";
        out << "outer loop\n";
        out << "vertex " << facet.v0x << ' ' << facet.v0y << ' ' << facet.v0z << "\n";
        out << "vertex " << facet.v1x << ' ' << facet.v1y << ' ' << facet.v1z << "\n";
        out << "vertex " << facet.v2x << ' ' << facet.v2y << ' ' << facet.v2z << "\n";
        out << "endloop\nendfacet\n";
    }
    out << "endsolid t\n";
    return StringToBytes(out.str());
}

// A right-hand-wound +Z triangle with a credible supplied normal.
RawFacet PlanarFacet(float offsetX)
{
    RawFacet facet;
    facet.nz = 1.0f;
    facet.v0x = offsetX;
    facet.v1x = offsetX + 1.0f;
    facet.v2y = 1.0f;
    return facet;
}

// A small closed-ish cube (12 facets) whose bounds the rasterizer can frame.
std::vector<RawFacet> CubeFacets()
{
    const float v[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                           {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                             {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    std::vector<RawFacet> facets;
    for (const auto& quad : quads) {
        const int i0 = quad[0];
        const int i1 = quad[1];
        const int i2 = quad[2];
        const int i3 = quad[3];
        RawFacet first, second;
        first.v0x = v[i0][0]; first.v0y = v[i0][1]; first.v0z = v[i0][2];
        first.v1x = v[i1][0]; first.v1y = v[i1][1]; first.v1z = v[i1][2];
        first.v2x = v[i2][0]; first.v2y = v[i2][1]; first.v2z = v[i2][2];
        second = first;
        second.v1x = v[i2][0]; second.v1y = v[i2][1]; second.v1z = v[i2][2];
        second.v2x = v[i3][0]; second.v2y = v[i3][1]; second.v2z = v[i3][2];
        facets.push_back(first);
        facets.push_back(second);
    }
    return facets;
}

ProviderOutcome RunPipeline(BoundedSource& source, std::uint32_t cx = 64)
{
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Stl;
    request.source = &source;
    request.limits = &ProviderLimits::Default();
    request.deadline = &deadline;
    request.ledger = &ledger;
    request.cx = cx;
    RasterImage image;
    return RunThumbnailPipeline(request, DefaultThumbnailDependencies(), image);
}

} // namespace

TEST_CASE("a valid binary STL emits normalized triangles and the neutral material",
          "[provider][stl]")
{
    const std::vector<RawFacet> facets{PlanarFacet(0.0f), PlanarFacet(2.0f)};
    ByteSource source(BuildBinaryStl(facets));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StlAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.initialize == ErrorCode::None);
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.materials == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE(result.materialIndices == std::vector<std::uint32_t>{1u});
    REQUIRE(sink.triangles.size() == 2u);
    REQUIRE(sink.points.empty());

    for (const TriangleSample& triangle : sink.triangles) {
        CHECK(triangle.materialIndex == 1u);
        for (const VertexSample& vertex : triangle.vertices) {
            CHECK(vertex.normal[2] == 1.0f); // credible supplied +Z normal
            CHECK(vertex.color[3] == 1.0f);
        }
    }
    CHECK(sink.triangles[0].vertices[0].position[0] == 0.0f);
    CHECK(sink.triangles[1].vertices[0].position[0] == 2.0f);
}

TEST_CASE("a valid binary STL renders through the routed pipeline without a contiguous view",
          "[provider][stl]")
{
    ByteSource source(BuildBinaryStl(CubeFacets()), /*exposeContiguous=*/false);
    CHECK(RunPipeline(source) == ProviderOutcome::Success);
}

TEST_CASE("a valid ASCII STL emits the same geometry as its binary form",
          "[provider][stl]")
{
    const std::vector<RawFacet> facets{PlanarFacet(0.0f), PlanarFacet(2.0f)};
    ByteSource source(BuildAsciiStl(facets));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StlAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE(sink.triangles.size() == 2u);
    for (const TriangleSample& triangle : sink.triangles) {
        CHECK(triangle.materialIndex == 1u);
        for (const VertexSample& vertex : triangle.vertices) {
            CHECK(vertex.normal[2] == 1.0f);
        }
    }
}

TEST_CASE("a binary-shaped source whose header starts with 'solid' is still parsed as binary",
          "[provider][stl]")
{
    std::vector<std::byte> bytes = BuildBinaryStl({PlanarFacet(0.0f)});
    std::memcpy(bytes.data(), "solid", 5); // free-form binary header text
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StlAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE(sink.triangles.size() == 1u);
    // The binary interpretation of PlanarFacet: a credible +Z supplied normal.
    CHECK(sink.triangles[0].vertices[0].normal[2] == 1.0f);
}

TEST_CASE("a declared facet count over the Tier A cap is a resource limit before allocation",
          "[provider][stl]")
{
    std::vector<std::byte> bytes(84, std::byte{0});
    const std::uint32_t overCap = parser_core::kMaxStlFacets + 1;
    std::memcpy(bytes.data() + 80, &overCap, sizeof(overCap));
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StlAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    // Rejected in Parse, before any facet buffer is sized from the count.
    CHECK(result.parse == ErrorCode::ResourceLimit);
    CHECK_FALSE(result.geometryRan); // EnumerateGeometry not reached
    CHECK(sink.triangles.empty());
}

TEST_CASE("a fabricated count inconsistent with the source length is malformed data",
          "[provider][stl]")
{
    std::vector<std::byte> bytes(84, std::byte{0});
    const std::uint32_t plausible = 5; // within the cap, but no facet bytes follow
    std::memcpy(bytes.data() + 80, &plausible, sizeof(plausible));
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StlAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::MalformedData);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a truncated binary file is malformed data", "[provider][stl]")
{
    std::vector<std::byte> bytes = BuildBinaryStl({PlanarFacet(0.0f), PlanarFacet(2.0f)});
    bytes.resize(bytes.size() - 25); // declares 2 facets, carries part of the second
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StlAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::MalformedData);
    CHECK(sink.triangles.empty());
}

TEST_CASE("non-finite facets are dropped while finite facets still render",
          "[provider][stl]")
{
    RawFacet nonFinite = PlanarFacet(0.0f);
    nonFinite.v0x = std::numeric_limits<float>::quiet_NaN();
    ByteSource source(BuildBinaryStl({nonFinite, PlanarFacet(4.0f)}));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StlAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.geometry == ErrorCode::None);
    REQUIRE(sink.triangles.size() == 1u);
    CHECK(sink.triangles[0].vertices[0].position[0] == 4.0f);
}

TEST_CASE("a file of only non-finite facets yields no geometry", "[provider][stl]")
{
    RawFacet nonFinite = PlanarFacet(0.0f);
    nonFinite.v2y = std::numeric_limits<float>::infinity();
    ByteSource source(BuildBinaryStl({nonFinite}));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StlAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a geometry-sink cap stop ends enumeration without failing", "[provider][stl]")
{
    const std::vector<RawFacet> facets{PlanarFacet(0.0f), PlanarFacet(2.0f), PlanarFacet(4.0f),
                                       PlanarFacet(6.0f)};
    ByteSource source(BuildBinaryStl(facets));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;
    sink.triangleLimit = 2;

    StlAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.size() == 2u);
}

TEST_CASE("an expired deadline stops the parse with a typed outcome", "[provider][stl]")
{
    ByteSource source(BuildBinaryStl({PlanarFacet(0.0f)}));
    Deadline deadline(Deadline::Clock::now() - std::chrono::seconds(10));
    AllocationLedger ledger;
    CollectSink sink;

    StlAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::Cancelled);
    CHECK(sink.triangles.empty());
}

TEST_CASE("the routed pipeline maps STL failures to the tabulated outcomes",
          "[provider][stl]")
{
    {
        std::vector<std::byte> bytes(84, std::byte{0});
        const std::uint32_t overCap = parser_core::kMaxStlFacets + 1;
        std::memcpy(bytes.data() + 80, &overCap, sizeof(overCap));
        ByteSource source(std::move(bytes));
        CHECK(RunPipeline(source) == ProviderOutcome::LimitExceeded);
    }
    {
        std::vector<std::byte> bytes = BuildBinaryStl({PlanarFacet(0.0f), PlanarFacet(2.0f)});
        bytes.resize(bytes.size() - 25);
        ByteSource source(std::move(bytes));
        CHECK(RunPipeline(source) == ProviderOutcome::BadFormat);
    }
}

TEST_CASE("a valid STL renders a non-empty bitmap through the real dependencies",
          "[provider][stl]")
{
    ByteSource source(BuildBinaryStl(CubeFacets()));
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Stl;
    request.source = &source;
    request.limits = &ProviderLimits::Default();
    request.deadline = &deadline;
    request.ledger = &ledger;
    request.cx = 64;
    RasterImage image;

    const ProviderOutcome outcome =
        RunThumbnailPipeline(request, DefaultThumbnailDependencies(), image);

    CHECK(outcome == ProviderOutcome::Success);
    CHECK(image.width != 0u);
    CHECK(image.height != 0u);
    CHECK_FALSE(image.bgraPremultiplied.empty());
}

TEST_CASE("STL is routed from its frozen CLSID", "[provider][stl]")
{
    CHECK(FamilyForClsid("{BFC86E1A-55C1-4C2D-AA36-3C25DECF30C9}") == Family::Stl);
    CHECK(CreateFamilyAdapter(Family::Stl) != nullptr);
}