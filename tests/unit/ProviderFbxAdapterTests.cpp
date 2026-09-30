// T31 FBX family adapter coverage.
//
// `FbxFamilyAdapter.cpp` and `FamilyAdapterRegistry.cpp` are compiled into
// Tests.Unit.exe, so these cases exercise the real adapter contract directly
// (Initialize -> Parse -> EnumerateMaterials -> EnumerateGeometry) and through
// the real routed pipeline (`RunThumbnailPipeline` + `DefaultThumbnailDependencies`).
// The provider-local pinned ufbx copy is linked here too.
//
// Fixtures come from the committed, redistributable FBX qualification corpus in
// `tests/fixtures/fbx-spike` (`PREVIEW3D_FBX_FIXTURE_DIR`). The provider itself
// never opens a path; the tests read the bytes and hand them to a bounded source.
// Binary fixtures are `.base64` text and are decoded in memory.

#include <catch2/catch_test_macros.hpp>

#include "AllocationLedger.h"
#include "Deadline.h"
#include "FamilyAdapter.h"
#include "FamilyAdapterRegistry.h"
#include "FamilyRouting.h"
#include "FbxFamilyAdapter.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"
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
    bool externalFile = false;
    bool evaluationRan = false;
    bool omittedUnsupported = false;
    std::uint64_t evaluatedTriangles = 0;
    std::uint64_t embeddedImages = 0;
    std::uint64_t embeddedPixels = 0;
    std::vector<std::uint32_t> materialIndices;
    std::vector<model_core::MaterialPayload> materials;
};

AdapterResult RunAdapter(FbxAdapter& adapter, BoundedSource& source, Deadline& deadline,
                         AllocationLedger& ledger, IGeometrySink& sink)
{
    AdapterInput input{};
    input.source = &source;
    input.limits = &ProviderLimits::Default();
    input.deadline = &deadline;
    input.ledger = &ledger;
    input.family = Family::Fbx;

    AdapterResult result;
    result.initialize = adapter.Initialize(input);
    if (result.initialize != ErrorCode::None) {
        return result;
    }
    result.parse = adapter.Parse();
    result.externalFile = adapter.ExternalFileDetected();
    result.evaluationRan = adapter.EvaluationRan();
    result.omittedUnsupported = adapter.OmittedUnsupportedGeometry();
    result.evaluatedTriangles = adapter.EvaluatedTriangles();
    result.embeddedImages = adapter.EmbeddedImageCount();
    result.embeddedPixels = adapter.EmbeddedImagePixels();
    if (result.parse != ErrorCode::None) {
        return result;
    }
    CollectMaterials materials;
    result.materialsError = adapter.EnumerateMaterials(materials);
    result.materialIndices = materials.indices;
    result.materials = materials.materials;
    result.geometryRan = true;
    result.geometry = adapter.EnumerateGeometry(sink);
    return result;
}

ProviderOutcome RunPipeline(BoundedSource& source, std::uint32_t cx = 64)
{
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Fbx;
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

std::vector<std::byte> Fixture(const char* name)
{
    return ReadFile(std::filesystem::path(PREVIEW3D_FBX_FIXTURE_DIR) / name);
}

int Base64Value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

std::vector<std::byte> DecodeBase64(const std::vector<std::byte>& text)
{
    std::vector<std::byte> out;
    int accumulator = 0;
    int bits = 0;
    for (const std::byte raw : text) {
        const char c = static_cast<char>(raw);
        if (c == '=' ) {
            break;
        }
        const int value = Base64Value(c);
        if (value < 0) {
            continue; // whitespace/newlines
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

std::vector<std::byte> BinaryFixture(const char* base64Name)
{
    return DecodeBase64(Fixture(base64Name));
}

bool AllFinite(const CollectSink& sink)
{
    for (const TriangleSample& triangle : sink.triangles) {
        for (const VertexSample& vertex : triangle.vertices) {
            for (int i = 0; i < 3; ++i) {
                if (!std::isfinite(vertex.position[i]) || !std::isfinite(triangle.origin[i])) {
                    return false;
                }
                if (!std::isfinite(vertex.normal[i]) || !std::isfinite(vertex.color[i])) {
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace

// --- Binary / ASCII ---------------------------------------------------------

TEST_CASE("a binary FBX cube parses and emits triangles", "[provider][fbx]")
{
    ByteSource source(BinaryFixture("cube-binary.fbx.base64"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    FbxAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.initialize == ErrorCode::None);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.evaluationRan);
    CHECK(result.materialsError == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE_FALSE(sink.triangles.empty());
    CHECK(sink.points.empty());
    CHECK(AllFinite(sink));
    // A binary cube exceeds the T14 sampler's single-triangle cap only at the
    // sampler, not here; all source triangles reach the sink.
    CHECK(sink.triangles.size() >= 12u);
}

TEST_CASE("an ASCII FBX hierarchy parses and emits every instance",
          "[provider][fbx]")
{
    ByteSource source(Fixture("hierarchy-instances-pivots-ascii.fbx"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    FbxAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE_FALSE(sink.triangles.empty());
    CHECK(AllFinite(sink));
    CHECK_FALSE(result.externalFile);
}

TEST_CASE("a skinned FBX is evaluated to a deterministic static pose", "[provider][fbx]")
{
    ByteSource source(Fixture("dual-quaternion-ascii.fbx"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    FbxAdapter adapter;
    const AdapterResult first = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(first.parse == ErrorCode::None);
    CHECK(first.evaluationRan);
    REQUIRE_FALSE(sink.triangles.empty());
    CHECK(AllFinite(sink));

    // The evaluated pose is deterministic across repeated parses of the same
    // bytes (no animation playback, fixed first-stack start time).
    ByteSource again(Fixture("dual-quaternion-ascii.fbx"));
    Deadline deadline2;
    AllocationLedger ledger2;
    CollectSink sink2;
    FbxAdapter adapter2;
    const AdapterResult second = RunAdapter(adapter2, again, deadline2, ledger2, sink2);
    REQUIRE(second.parse == ErrorCode::None);
    REQUIRE(sink2.triangles.size() == sink.triangles.size());
    CHECK(sink2.triangles[0].vertices[0].position[0]
          == sink.triangles[0].vertices[0].position[0]);
}

// --- Embedded / external media ----------------------------------------------

TEST_CASE("an FBX with an embedded image validates it and still renders geometry",
          "[provider][fbx]")
{
    ByteSource source(Fixture("embedded-png-ascii.fbx"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    FbxAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(result.embeddedImages >= 1u);
    CHECK(result.embeddedPixels > 0u);
    // An optional image can never fail valid geometry.
    REQUIRE_FALSE(sink.triangles.empty());
}

TEST_CASE("an FBX with external textures renders its geometry with the neutral fallback",
          "[provider][fbx]")
{
    ByteSource source(Fixture("layered-textures-ascii.fbx"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    FbxAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE_FALSE(sink.triangles.empty());
    // The provider never asks the filesystem for a sidecar.
    CHECK_FALSE(result.externalFile);
}

TEST_CASE("a NURBS-only FBX is an unsupported required feature, not a crash",
          "[provider][fbx]")
{
    ByteSource source(Fixture("nurbs-only-ascii.fbx"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    FbxAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    CHECK(result.parse == ErrorCode::UnsupportedRequiredFeature);
    CHECK(sink.triangles.empty());
    CHECK(RunPipeline(source) == ProviderOutcome::Unsupported);
}

// --- Malformed / limit / deadline -------------------------------------------

TEST_CASE("a truncated binary FBX is a typed failure", "[provider][fbx]")
{
    std::vector<std::byte> bytes = BinaryFixture("cube-binary.fbx.base64");
    REQUIRE(bytes.size() > 64);
    bytes.resize(bytes.size() / 2);
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    FbxAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse != ErrorCode::None);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a non-FBX stream is rejected even though the CLSID routes to FBX",
          "[provider][fbx]")
{
    const std::string text = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    ByteSource source(std::move(bytes));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    FbxAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::UnsupportedFormat);
}

TEST_CASE("an expired deadline stops the parse with a typed outcome", "[provider][fbx]")
{
    ByteSource source(Fixture("hierarchy-instances-pivots-ascii.fbx"));
    Deadline deadline(Deadline::Clock::now() - std::chrono::seconds(10));
    AllocationLedger ledger;
    CollectSink sink;

    FbxAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::Cancelled);
    CHECK(sink.triangles.empty());
}

TEST_CASE("an FBX sink cap stop ends enumeration without failing", "[provider][fbx]")
{
    ByteSource source(BinaryFixture("cube-binary.fbx.base64"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;
    sink.triangleLimit = 0;

    FbxAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a bounded backing over the ledger cap is a resource limit", "[provider][fbx]")
{
    // No contiguous view forces the checked owned backing; a zero-capacity
    // ledger refuses the charge before any byte is copied.
    ByteSource source(BinaryFixture("cube-binary.fbx.base64"), /*exposeContiguous=*/false);
    Deadline deadline;
    AllocationLedger ledger(0);
    CollectSink sink;

    FbxAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::ResourceLimit);
}

TEST_CASE("a non-contiguous FBX source parses through the bounded backing buffer",
          "[provider][fbx]")
{
    ByteSource source(Fixture("hierarchy-instances-pivots-ascii.fbx"),
                      /*exposeContiguous=*/false);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    FbxAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
}

// --- Routing / rendering ----------------------------------------------------

TEST_CASE("the routed pipeline maps FBX failures to the tabulated outcomes",
          "[provider][fbx]")
{
    const std::string text = "not an fbx stream";
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    ByteSource source(std::move(bytes));
    CHECK(RunPipeline(source) == ProviderOutcome::BadFormat);
}

TEST_CASE("a valid FBX renders a non-empty bitmap through the real dependencies",
          "[provider][fbx]")
{
    ByteSource source(Fixture("hierarchy-instances-pivots-ascii.fbx"));
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Fbx;
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

TEST_CASE("FBX is routed from its frozen CLSID", "[provider][fbx]")
{
    CHECK(FamilyForClsid("{FBC218D4-FD2C-41DF-B168-7F3B9E53C84E}") == Family::Fbx);
    CHECK(CreateFamilyAdapter(Family::Fbx) != nullptr);
}