// T33 USD/USDZ family adapter coverage.
//
// `UsdFamilyAdapter.cpp`, `UsdZipPreflight.cpp` and `FamilyAdapterRegistry.cpp`
// are compiled into Tests.Unit.exe, so these cases exercise the real adapter
// contract directly (Initialize -> Parse -> EnumerateMaterials ->
// EnumerateGeometry) and through the real routed pipeline
// (`RunThumbnailPipeline` + `DefaultThumbnailDependencies`). The provider-local
// pinned TinyUSDZ reader is linked here too.
//
// The fixtures come from the committed, redistributable USD-001/USD-004/USD-005
// corpus in `tests/fixtures/usd-spike` (`PREVIEW3D_USD_FIXTURE_DIR`). The
// provider itself never opens a path; the tests read the bytes once and hand
// them to a bounded source. The two binary fixtures are `.base64` text and are
// decoded in memory.

#include <catch2/catch_test_macros.hpp>

#include "AllocationLedger.h"
#include "Deadline.h"
#include "FamilyAdapter.h"
#include "FamilyAdapterRegistry.h"
#include "FamilyRouting.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"
#include "ThumbnailPipeline.h"
#include "UsdFamilyAdapter.h"

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
    std::uint64_t meshes = 0;
    std::uint64_t nodes = 0;
    std::uint64_t materials = 0;
    std::uint64_t inspectedTriangles = 0;
    std::uint64_t instances = 0;
    bool composition = false;
    bool externalAsset = false;
    bool usdz = false;
    std::vector<std::uint32_t> materialIndices;
    std::vector<model_core::MaterialPayload> materialPayloads;
};

AdapterResult RunAdapter(UsdAdapter& adapter, BoundedSource& source, Deadline& deadline,
                         AllocationLedger& ledger, IGeometrySink& sink)
{
    AdapterInput input{};
    input.source = &source;
    input.limits = &ProviderLimits::Default();
    input.deadline = &deadline;
    input.ledger = &ledger;
    input.family = Family::Usd;

    AdapterResult result;
    result.initialize = adapter.Initialize(input);
    if (result.initialize != ErrorCode::None) {
        return result;
    }
    result.parse = adapter.Parse();
    result.meshes = adapter.MeshCount();
    result.nodes = adapter.NodeCount();
    result.materials = adapter.MaterialCount();
    result.composition = adapter.DetectedComposition();
    result.externalAsset = adapter.DetectedExternalAsset();
    result.usdz = adapter.UsedUsdzArchive();
    if (result.parse != ErrorCode::None) {
        return result;
    }
    CollectMaterials materials;
    result.materialsError = adapter.EnumerateMaterials(materials);
    result.materialIndices = materials.indices;
    result.materialPayloads = materials.materials;
    result.geometryRan = true;
    result.geometry = adapter.EnumerateGeometry(sink);
    // Instance and inspected-triangle counters are populated by enumeration.
    result.instances = adapter.InstanceCount();
    result.inspectedTriangles = adapter.InspectedTriangleCount();
    return result;
}

ProviderOutcome RunPipeline(BoundedSource& source, std::uint32_t cx = 64)
{
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Usd;
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
    const std::filesystem::path path =
        std::filesystem::path(PREVIEW3D_USD_FIXTURE_DIR) / name;
    if (path.extension() == ".base64") {
        return DecodeBase64(ReadFile(path));
    }
    return ReadFile(path);
}

std::vector<std::byte> Ascii(std::string_view text)
{
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
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

// --- USDA / USDC / USDZ -----------------------------------------------------

TEST_CASE("a stream-contained USDA mesh parses and emits triangles", "[provider][usd]")
{
    ByteSource source(Fixture("mesh.usda"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.initialize == ErrorCode::None);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.meshes == 1u);
    CHECK_FALSE(result.usdz);
    CHECK_FALSE(result.composition);
    CHECK_FALSE(result.externalAsset);
    // The quad is triangulated into two triangles and the neutral display-color
    // path seeds one white material index.
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE(sink.triangles.size() == 2u);
    CHECK(sink.points.empty());
    CHECK(AllFinite(sink));
    CHECK(result.materialIndices.size() == 1u);
    CHECK(RunPipeline(source) == ProviderOutcome::Success);
}

TEST_CASE("a crate-encoded USDC mesh parses from byte-sniffed input", "[provider][usd]")
{
    ByteSource source(Fixture("cube.usdc.base64"));
    REQUIRE(source.Size() > 8u);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.parse == ErrorCode::None);
    CHECK_FALSE(result.usdz);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE_FALSE(sink.triangles.empty());
    CHECK(AllFinite(sink));
    CHECK(RunPipeline(source) == ProviderOutcome::Success);
}

TEST_CASE("a contained USDZ archive renders without extracting a path", "[provider][usd]")
{
    ByteSource source(Fixture("cube.usdz.base64"));
    REQUIRE(source.Size() > 4u);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.usdz);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE_FALSE(sink.triangles.empty());
    CHECK(AllFinite(sink));

    RasterImage image;
    Deadline imageDeadline;
    AllocationLedger imageLedger;
    ThumbnailRequest request{};
    request.family = Family::Usd;
    request.source = &source;
    request.limits = &ProviderLimits::Default();
    request.deadline = &imageDeadline;
    request.ledger = &imageLedger;
    request.cx = 64;
    CHECK(RunThumbnailPipeline(request, DefaultThumbnailDependencies(), image)
          == ProviderOutcome::Success);
    CHECK(image.width != 0u);
    CHECK(image.height != 0u);
    CHECK_FALSE(image.bgraPremultiplied.empty());
}

// --- Static scene / point instancers ---------------------------------------

TEST_CASE("a static USD scene applies time, purpose and visibility policy", "[provider][usd]")
{
    ByteSource source(Fixture("static-scene.usda"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    // Two authored instances with one invisible id; the guide-purpose mesh is
    // omitted but its omission does not erase the visible sampled scene.
    CHECK(result.instances == 1u);
    REQUIRE_FALSE(sink.triangles.empty());
    CHECK(AllFinite(sink));
}

TEST_CASE("a self-contained point instancer renders its visible instances", "[provider][usd]")
{
    ByteSource source(Fixture("compat-point-instancer.usda"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(result.instances == 2u);
    REQUIRE_FALSE(sink.triangles.empty());
}

// --- Fallbacks --------------------------------------------------------------

TEST_CASE("a USD layer with an external texture falls back to the generic icon",
          "[provider][usd]")
{
    ByteSource source(Fixture("materials.usda"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    // The provider must never resolve @albedo.png@; it fails closed.
    CHECK(result.parse == ErrorCode::UnsafeReference);
    CHECK(result.externalAsset);
    CHECK(sink.triangles.empty());
    CHECK(RunPipeline(source) == ProviderOutcome::Unsupported);
}

TEST_CASE("a composed USD stage falls back to the generic icon", "[provider][usd]")
{
    ByteSource source(Fixture("compat-composition.usda"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    // Sublayers/references/payloads/inherits/specializes/variants are all
    // composition the stream-only provider does not perform.
    CHECK(result.parse == ErrorCode::UnsupportedComposition);
    CHECK(result.composition);
    CHECK(sink.triangles.empty());
    CHECK(RunPipeline(source) == ProviderOutcome::Unsupported);
}

TEST_CASE("a malformed USDA layer is a typed failure", "[provider][usd]")
{
    ByteSource source(Ascii("#usda 1.0\n( \n  this is not valid"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse != ErrorCode::None);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a non-USD stream is rejected even though the CLSID routes to USD",
          "[provider][usd]")
{
    ByteSource source(Ascii("not a usd stage at all"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::MalformedData);
    CHECK(RunPipeline(source) == ProviderOutcome::BadFormat);
}

TEST_CASE("an empty stream is a typed failure", "[provider][usd]")
{
    ByteSource source({});
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse != ErrorCode::None);
}

TEST_CASE("a truncated USDZ archive is a typed failure", "[provider][usd]")
{
    std::vector<std::byte> bytes = Fixture("cube.usdz.base64");
    REQUIRE(bytes.size() > 64u);
    bytes.resize(bytes.size() / 2);
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse != ErrorCode::None);
    CHECK(sink.triangles.empty());
}

// --- Limits / deadline / backing -------------------------------------------

TEST_CASE("an expired deadline stops the parse with a typed outcome", "[provider][usd]")
{
    ByteSource source(Fixture("mesh.usda"));
    Deadline deadline(Deadline::Clock::now() - std::chrono::seconds(10));
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::Cancelled);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a USD sink cap stop ends enumeration without failing", "[provider][usd]")
{
    ByteSource source(Fixture("mesh.usda"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;
    sink.triangleLimit = 0;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a bounded backing over the ledger cap is a resource limit", "[provider][usd]")
{
    ByteSource source(Fixture("mesh.usda"), /*exposeContiguous=*/false);
    Deadline deadline;
    AllocationLedger ledger(0);
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::ResourceLimit);
}

TEST_CASE("a non-contiguous USD source parses through the bounded backing buffer",
          "[provider][usd]")
{
    ByteSource source(Fixture("mesh.usda"), /*exposeContiguous=*/false);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    UsdAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
}

TEST_CASE("a valid USD render produces a non-empty bitmap through the real dependencies",
          "[provider][usd]")
{
    ByteSource source(Fixture("mesh.usda"));
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Usd;
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

TEST_CASE("USD is routed from its frozen CLSID", "[provider][usd]")
{
    CHECK(FamilyForClsid("{E938BC70-4C08-4446-A15D-EE31576BFB48}") == Family::Usd);
    CHECK(CreateFamilyAdapter(Family::Usd) != nullptr);
}