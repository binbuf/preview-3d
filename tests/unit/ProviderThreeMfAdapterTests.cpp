// T32 3MF family adapter coverage.
//
// `ThreeMfFamilyAdapter.cpp`, `ThreeMfOpcPreflight.cpp` and
// `FamilyAdapterRegistry.cpp` are compiled into Tests.Unit.exe, so these cases
// exercise the real adapter contract directly (Initialize -> Parse ->
// EnumerateMaterials -> EnumerateGeometry) and through the real routed pipeline
// (`RunThumbnailPipeline` + `DefaultThumbnailDependencies`). The provider-local
// pinned lib3mf reader is linked here too.
//
// The valid fixtures come from the committed, redistributable 3MF-007 corpus in
// `tests/fixtures/3mf-spike` (`PREVIEW3D_3MF_FIXTURE_DIR`). The provider itself
// never opens a path; the tests read the bytes and hand them to a bounded
// source. Binary fixtures are `.base64` text and are decoded in memory.

#include <catch2/catch_test_macros.hpp>

#include "AllocationLedger.h"
#include "Deadline.h"
#include "FamilyAdapter.h"
#include "FamilyAdapterRegistry.h"
#include "FamilyRouting.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"
#include "ThreeMfFamilyAdapter.h"
#include "ThumbnailPipeline.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    ErrorCode materialsError = ErrorCode::None;
    ErrorCode geometry = ErrorCode::None;
    bool geometryRan = false;
    std::uint64_t buildItems = 0;
    std::uint64_t occurrences = 0;
    std::uint64_t inspectedTriangles = 0;
    std::uint64_t embeddedImages = 0;
    std::uint64_t embeddedPixels = 0;
    std::uint64_t latticeTriangles = 0;
    bool usedLattice = false;
    std::vector<std::uint32_t> materialIndices;
    std::vector<model_core::MaterialPayload> materials;
};

AdapterResult RunAdapter(ThreeMfAdapter& adapter, BoundedSource& source, Deadline& deadline,
                         AllocationLedger& ledger, IGeometrySink& sink)
{
    AdapterInput input{};
    input.source = &source;
    input.limits = &ProviderLimits::Default();
    input.deadline = &deadline;
    input.ledger = &ledger;
    input.family = Family::ThreeMf;

    AdapterResult result;
    result.initialize = adapter.Initialize(input);
    if (result.initialize != ErrorCode::None) {
        return result;
    }
    result.parse = adapter.Parse();
    result.buildItems = adapter.BuildItemCount();
    result.occurrences = adapter.OccurrenceCount();
    result.inspectedTriangles = adapter.InspectedTriangleCount();
    if (result.parse != ErrorCode::None) {
        return result;
    }
    CollectMaterials materials;
    result.materialsError = adapter.EnumerateMaterials(materials);
    result.materialIndices = materials.indices;
    result.materials = materials.materials;
    result.geometryRan = true;
    result.geometry = adapter.EnumerateGeometry(sink);
    // The color/texture and lattice hooks are populated during enumeration.
    result.embeddedImages = adapter.EmbeddedImageCount();
    result.embeddedPixels = adapter.EmbeddedImagePixels();
    result.latticeTriangles = adapter.LatticeTriangleCount();
    result.usedLattice = adapter.UsedLattice();
    return result;
}

ProviderOutcome RunPipeline(BoundedSource& source, std::uint32_t cx = 64)
{
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::ThreeMf;
    request.source = &source;
    request.limits = &ProviderLimits::Default();
    request.deadline = &deadline;
    request.ledger = &ledger;
    request.cx = cx;
    RasterImage image;
    return RunThumbnailPipeline(request, DefaultThumbnailDependencies(), image);
}

// --- Fixture loading --------------------------------------------------------

std::vector<std::byte> ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

int Base64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<std::byte> DecodeBase64(const std::vector<std::byte>& text)
{
    std::vector<std::byte> out;
    int accumulator = 0;
    int bits = 0;
    for (const std::byte raw : text) {
        const char c = static_cast<char>(raw);
        if (c == '=') {
            break;
        }
        const int value = Base64Value(c);
        if (value < 0) {
            continue;
        }
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((accumulator >> bits) & 0xFF));
        }
    }
    return out;
}

std::vector<std::byte> Fixture(const char* name)
{
    return DecodeBase64(ReadFile(std::filesystem::path(PREVIEW3D_3MF_FIXTURE_DIR) / name));
}

bool AllFinite(const CollectSink& sink)
{
    for (const TriangleSample& triangle : sink.triangles) {
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(triangle.origin[i])) {
                return false;
            }
        }
        for (const VertexSample& vertex : triangle.vertices) {
            for (int i = 0; i < 3; ++i) {
                if (!std::isfinite(vertex.position[i]) || !std::isfinite(vertex.normal[i])
                    || !std::isfinite(vertex.color[i])) {
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace

// --- Core / Production ------------------------------------------------------

TEST_CASE("a Core 3MF box parses and emits triangles", "[provider][3mf]")
{
    ByteSource source(Fixture("core-box.3mf.base64"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.initialize == ErrorCode::None);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.buildItems == 1u);
    CHECK(result.occurrences == 1u);
    CHECK(result.materialIndices.size() == 1u);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE_FALSE(sink.triangles.empty());
    CHECK(sink.points.empty());
    CHECK(AllFinite(sink));
    CHECK(sink.triangles.size() == 48u);
    // The box has no per-object color, so it uses the neutral 0.8 default.
    CHECK(sink.triangles[0].vertices[0].color[0] == 0.8f);
    CHECK(RunPipeline(source) == ProviderOutcome::Success);
}

TEST_CASE("a nested-component 3MF flattens its root build", "[provider][3mf]")
{
    ByteSource source(Fixture("nested-components.3mf.base64"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(result.occurrences >= 1u);
    REQUIRE_FALSE(sink.triangles.empty());
    CHECK(AllFinite(sink));
}

TEST_CASE("a static Production multi-part package renders its root build",
          "[provider][3mf]")
{
    ByteSource source(Fixture("static-production.3mf.base64"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.buildItems >= 2u);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE_FALSE(sink.triangles.empty());
    CHECK(AllFinite(sink));
}

// --- Materials / textures ---------------------------------------------------

TEST_CASE("a textured 3MF validates the contained image and renders geometry",
          "[provider][3mf]")
{
    ByteSource source(Fixture("materials-texture.3mf.base64"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    // The frozen provider material contract has no texture slot, so the image
    // is validated against the aggregate pixel budget and not decoded.
    CHECK(result.embeddedImages >= 1u);
    CHECK(result.embeddedPixels > 0u);
    REQUIRE_FALSE(sink.triangles.empty());
    CHECK(AllFinite(sink));
}

// --- Beam lattice -----------------------------------------------------------

TEST_CASE("a bounded Beam Lattice renders a complete tessellated preview",
          "[provider][3mf]")
{
    ByteSource source(Fixture("beam-lattice.3mf.base64"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(result.usedLattice);
    CHECK(result.latticeTriangles > 0u);
    REQUIRE_FALSE(sink.triangles.empty());
    CHECK(AllFinite(sink));
    CHECK(sink.triangles.size() <= ProviderLimits::kTrianglesInspectedMax);
}

TEST_CASE("an outside-clipped lattice without a representation mesh is unsupported",
          "[provider][3mf]")
{
    ByteSource source(Fixture("beam-representation.3mf.base64"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::UnsupportedRequiredFeature);
    CHECK(sink.triangles.empty());
    CHECK(RunPipeline(source) == ProviderOutcome::Unsupported);
}

// --- Required extension / malformed / limit / deadline ----------------------

TEST_CASE("an unsupported required extension falls back to the generic icon",
          "[provider][3mf]")
{
    // production-boxes declares Slice as required in every model part.
    ByteSource source(Fixture("production-boxes.3mf.base64"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::UnsupportedRequiredFeature);
    CHECK(sink.triangles.empty());
    CHECK(RunPipeline(source) == ProviderOutcome::Unsupported);
}

TEST_CASE("a truncated 3MF is a typed failure", "[provider][3mf]")
{
    std::vector<std::byte> bytes = Fixture("core-box.3mf.base64");
    REQUIRE(bytes.size() > 64);
    bytes.resize(bytes.size() / 2);
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse != ErrorCode::None);
    CHECK(sink.triangles.empty());
}

TEST_CASE("an empty stream is a typed failure", "[provider][3mf]")
{
    ByteSource source({});
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse != ErrorCode::None);
}

TEST_CASE("a non-3MF stream is rejected even though the CLSID routes to 3MF",
          "[provider][3mf]")
{
    const std::string text = "not a 3mf package";
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse != ErrorCode::None);
    CHECK(RunPipeline(source) == ProviderOutcome::BadFormat);
}

TEST_CASE("an expired deadline stops the parse with a typed outcome", "[provider][3mf]")
{
    ByteSource source(Fixture("core-box.3mf.base64"));
    Deadline deadline(Deadline::Clock::now() - std::chrono::seconds(10));
    AllocationLedger ledger;
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::Cancelled);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a 3MF sink cap stop ends enumeration without failing", "[provider][3mf]")
{
    ByteSource source(Fixture("core-box.3mf.base64"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;
    sink.triangleLimit = 0;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a bounded backing over the ledger cap is a resource limit", "[provider][3mf]")
{
    ByteSource source(Fixture("core-box.3mf.base64"), /*exposeContiguous=*/false);
    Deadline deadline;
    AllocationLedger ledger(0);
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::ResourceLimit);
}

TEST_CASE("a non-contiguous 3MF source parses through the bounded backing buffer",
          "[provider][3mf]")
{
    ByteSource source(Fixture("core-box.3mf.base64"), /*exposeContiguous=*/false);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    ThreeMfAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
}

TEST_CASE("a valid 3MF renders a non-empty bitmap through the real dependencies",
          "[provider][3mf]")
{
    ByteSource source(Fixture("beam-lattice.3mf.base64"));
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::ThreeMf;
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

// --- Routing ----------------------------------------------------------------

TEST_CASE("3MF is routed from its frozen CLSID", "[provider][3mf]")
{
    CHECK(FamilyForClsid("{D8389A63-8526-454A-9892-72F3149484B9}") == Family::ThreeMf);
    CHECK(CreateFamilyAdapter(Family::ThreeMf) != nullptr);
}