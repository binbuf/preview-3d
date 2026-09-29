// T24 OBJ family adapter coverage.
//
// `ObjFamilyAdapter.cpp` and `FamilyAdapterRegistry.cpp` are compiled into
// Tests.Unit.exe, so these cases exercise the real adapter contract directly
// (Initialize -> Parse -> EnumerateMaterials -> EnumerateGeometry) and through
// the real routed pipeline (`RunThumbnailPipeline` + `DefaultThumbnailDependencies`,
// i.e. the shipped T14 sampler and T15 rasterizer). The provider-local pinned
// ufbx copy is linked here too, so an MTL/texture-referencing OBJ is shown to
// render neutrally without the adapter ever requesting a file open.
//
// Fixtures are ASCII text built in-memory (no committed model file).

#include <catch2/catch_test_macros.hpp>

#include "AllocationLedger.h"
#include "Deadline.h"
#include "FamilyAdapter.h"
#include "FamilyAdapterRegistry.h"
#include "FamilyRouting.h"
#include "ObjFamilyAdapter.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"
#include "ThumbnailPipeline.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace preview3d::provider;

namespace {

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
            return false;
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
    int triangleLimit = -1;

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
    bool externalFileDenied = false;
    std::vector<std::uint32_t> materialIndices;
    model_core::MaterialPayload material{};
};

std::vector<std::byte> Bytes(const std::string& text)
{
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
}

AdapterResult RunAdapter(ObjAdapter& adapter, BoundedSource& source, Deadline& deadline,
                         AllocationLedger& ledger, IGeometrySink& sink)
{
    AdapterInput input{};
    input.source = &source;
    input.limits = &ProviderLimits::Default();
    input.deadline = &deadline;
    input.ledger = &ledger;
    input.family = Family::Obj;

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
    if (!materials.materials.empty()) {
        result.material = materials.materials.front();
    }
    result.geometryRan = true;
    result.geometry = adapter.EnumerateGeometry(sink);
    result.externalFileDenied = adapter.ExternalFileDenied();
    return result;
}

ProviderOutcome RunPipeline(BoundedSource& source, std::uint32_t cx = 64)
{
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Obj;
    request.source = &source;
    request.limits = &ProviderLimits::Default();
    request.deadline = &deadline;
    request.ledger = &ledger;
    request.cx = cx;
    RasterImage image;
    return RunThumbnailPipeline(request, DefaultThumbnailDependencies(), image);
}

// --- Fixture text -----------------------------------------------------------

std::string TriangleObj()
{
    return "# T24 fixture\n"
           "v 0 0 0\n"
           "v 2 0 0\n"
           "v 0 2 0\n"
           "f 1 2 3\n";
}

std::string CubeObj()
{
    return "o cube\n"
           "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
           "v 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
           "f 1 4 3 2\n"
           "f 5 6 7 8\n"
           "f 1 2 6 5\n"
           "f 4 8 7 3\n"
           "f 1 5 8 4\n"
           "f 2 3 7 6\n";
}

std::string TwoObjectsObj()
{
    return "o first\n"
           "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
           "o second\n"
           "v 2 0 0\nv 3 0 0\nv 2 1 0\nf 4 5 6\n";
}

std::string ColoredObj()
{
    return "# vertex colors: v x y z r g b\n"
           "v 0 0 0 1 0 0\n"
           "v 2 0 0 0 1 0\n"
           "v 0 2 0 0 0 1\n"
           "f 1 2 3\n";
}

std::string MtlReferencingObj()
{
    return "mtllib missing-sidecar.mtl\n"
           "o part\n"
           "usemtl marked\n"
           "v 0 0 0\nv 2 0 0\nv 0 2 0\n"
           "f 1 2 3\n";
}

std::string UvObj()
{
    return "v 0 0 0\nv 2 0 0\nv 0 2 0\n"
           "vt 0 0\nvt 1 0\nvt 0 1\n"
           "f 1/1 2/2 3/3\n";
}

} // namespace

// --- Geometry ---------------------------------------------------------------

TEST_CASE("an ASCII OBJ triangle mesh emits one triangle and the neutral material",
          "[provider][obj]")
{
    ByteSource source(Bytes(TriangleObj()));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ObjAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.initialize == ErrorCode::None);
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.materials == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE(result.materialIndices == std::vector<std::uint32_t>{1u});
    REQUIRE(sink.triangles.size() == 1u);
    CHECK(sink.points.empty());
    CHECK(sink.triangles[0].materialIndex == 1u);
    CHECK(sink.triangles[0].vertices[0].position[0] == 0.0f);
    CHECK(sink.triangles[0].vertices[1].position[0] == 2.0f);
    CHECK(sink.triangles[0].vertices[2].position[1] == 2.0f);
    // Neutral material preserves the 0.72 base color when no vertex colors exist.
    CHECK(result.material.baseColorFactor[0] > 0.71f);
    CHECK(result.material.baseColorFactor[0] < 0.73f);
}

TEST_CASE("an OBJ quad mesh is fan-triangulated", "[provider][obj]")
{
    ByteSource source(Bytes(CubeObj()));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ObjAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    // 6 quads, two triangles each.
    CHECK(sink.triangles.size() == 12u);
}

TEST_CASE("an OBJ with multiple objects emits every object's geometry", "[provider][obj]")
{
    ByteSource source(Bytes(TwoObjectsObj()));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ObjAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE(sink.triangles.size() == 2u);
    bool sawSecond = false;
    for (const TriangleSample& triangle : sink.triangles) {
        if (triangle.vertices[0].position[0] >= 1.9f) {
            sawSecond = true;
        }
    }
    CHECK(sawSecond);
}

TEST_CASE("an OBJ with UVs still renders its triangles", "[provider][obj]")
{
    ByteSource source(Bytes(UvObj()));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ObjAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.size() == 1u);
}

TEST_CASE("vertex colors are carried and force a white base color", "[provider][obj]")
{
    ByteSource source(Bytes(ColoredObj()));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ObjAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    REQUIRE(sink.triangles.size() == 1u);
    CHECK(sink.triangles[0].vertices[0].color[0] == 1.0f);
    CHECK(sink.triangles[0].vertices[0].color[1] == 0.0f);
    CHECK(sink.triangles[0].vertices[1].color[1] == 1.0f);
    CHECK(sink.triangles[0].vertices[2].color[2] == 1.0f);
    // White base so `vertexColor * baseColor` preserves the source color.
    CHECK(result.material.baseColorFactor[0] == 1.0f);
    CHECK(result.material.baseColorFactor[1] == 1.0f);
    CHECK(result.material.baseColorFactor[2] == 1.0f);
}

// --- Isolation --------------------------------------------------------------

TEST_CASE("an OBJ referencing an MTL renders neutrally and never opens a sidecar",
          "[provider][obj]")
{
    ByteSource source(Bytes(MtlReferencingObj()));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ObjAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE(sink.triangles.size() == 1u);
    CHECK(sink.triangles[0].materialIndex == 1u);
    // Neutral material: the named `.mtl` is ignored entirely.
    CHECK(result.material.baseColorFactor[0] > 0.71f);
    CHECK(result.material.baseColorFactor[0] < 0.73f);
    // `load_external_files = false` means ufbx never even requested the `.mtl`.
    CHECK_FALSE(result.externalFileDenied);
}

TEST_CASE("a non-contiguous OBJ source parses through the bounded backing buffer",
          "[provider][obj]")
{
    ByteSource source(Bytes(TriangleObj()), /*exposeContiguous=*/false);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ObjAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.size() == 1u);
}

// --- Stop / deadline / typed failures ---------------------------------------

TEST_CASE("an OBJ sink cap stop ends enumeration without failing", "[provider][obj]")
{
    ByteSource source(Bytes(CubeObj()));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;
    sink.triangleLimit = 0;

    ObjAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.empty());
}

TEST_CASE("an expired deadline stops the parse with a typed outcome", "[provider][obj]")
{
    ByteSource source(Bytes(CubeObj()));
    Deadline deadline(Deadline::Clock::now() - std::chrono::seconds(10));
    AllocationLedger ledger;
    CollectSink sink;

    ObjAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::Cancelled);
    CHECK(sink.triangles.empty());
}

TEST_CASE("text that is not OBJ geometry is a typed failure", "[provider][obj]")
{
    ByteSource source(Bytes("this is not a model file\n"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ObjAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse != ErrorCode::None);
}

TEST_CASE("a truncated OBJ vertex line is a typed failure", "[provider][obj]")
{
    ByteSource source(Bytes("v 0 0\nf 1 2 3\n"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ObjAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse != ErrorCode::None);
}

TEST_CASE("an OBJ face above the per-face triangle ceiling is a resource limit",
          "[provider][obj]")
{
    // 65539 vertices and one 65539-gon: 65537 triangles, above the adapter's
    // 65536 ceiling, so enumeration refuses to build an unbounded polygon.
    std::string text;
    constexpr int kVertices = 65539;
    for (int i = 0; i < kVertices; ++i) {
        text += "v " + std::to_string(i) + " 0 0\n";
    }
    text += "f";
    for (int i = 1; i <= kVertices; ++i) {
        text += " " + std::to_string(i);
    }
    text += "\n";

    ByteSource source(Bytes(text));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ObjAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::ResourceLimit);
}

// --- Routing / rendering ----------------------------------------------------

TEST_CASE("the routed pipeline maps OBJ failures to the tabulated outcomes",
          "[provider][obj]")
{
    ByteSource source(Bytes("this is not a model file\n"));
    CHECK(RunPipeline(source) == ProviderOutcome::BadFormat);
}

TEST_CASE("a valid OBJ renders a non-empty bitmap through the real dependencies",
          "[provider][obj]")
{
    ByteSource source(Bytes(CubeObj()));
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Obj;
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

TEST_CASE("OBJ is routed from its frozen CLSID", "[provider][obj]")
{
    CHECK(FamilyForClsid("{D4722752-C480-4D9C-BEBE-1A9B514A8846}") == Family::Obj);
    CHECK(CreateFamilyAdapter(Family::Obj) != nullptr);
}