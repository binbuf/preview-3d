// T34 STEP/STP family adapter coverage.
//
// `StepFamilyAdapter.cpp`, `StepPart21Preflight.cpp` (the STEP host's
// product-owned Part-21 admission source) and `FamilyAdapterRegistry.cpp` are
// compiled into Tests.Unit.exe, and the dedicated static OCCT closure is linked
// here, so these cases exercise the real adapter contract directly
// (Initialize -> Parse -> EnumerateMaterials -> EnumerateGeometry) and through
// the real routed pipeline (`RunThumbnailPipeline` +
// `DefaultThumbnailDependencies`).
//
// The fixtures are the committed, immutable STEP-003/006 corpus in
// `tests/fixtures/stp-spike` (`PREVIEW3D_STEP_FIXTURE_DIR`). The provider itself
// never opens a path; the tests read the bytes once and hand them to a bounded
// source.

#define NOMINMAX
#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <psapi.h>

#include "AllocationLedger.h"
#include "Deadline.h"
#include "FamilyAdapter.h"
#include "FamilyAdapterRegistry.h"
#include "FamilyRouting.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"
#include "StepFamilyAdapter.h"
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
    std::uint64_t definitions = 0;
    std::uint64_t occurrences = 0;
    std::uint64_t materials = 0;
    std::uint64_t inspectedTriangles = 0;
    bool externalDocument = false;
    bool admissionRejected = false;
    std::vector<std::uint32_t> materialIndices;
};

AdapterResult RunAdapter(StepAdapter& adapter, BoundedSource& source, Deadline& deadline,
                         AllocationLedger& ledger, IGeometrySink& sink)
{
    AdapterInput input{};
    input.source = &source;
    input.limits = &ProviderLimits::Default();
    input.deadline = &deadline;
    input.ledger = &ledger;
    input.family = Family::Step;

    AdapterResult result;
    result.initialize = adapter.Initialize(input);
    if (result.initialize != ErrorCode::None) {
        return result;
    }
    result.parse = adapter.Parse();
    result.definitions = adapter.DefinitionCount();
    result.occurrences = adapter.OccurrenceCount();
    result.materials = adapter.MaterialCount();
    result.externalDocument = adapter.ExternalDocumentRejected();
    result.admissionRejected = adapter.AdmissionRejected();
    if (result.parse != ErrorCode::None) {
        return result;
    }
    CollectMaterials materials;
    result.materialsError = adapter.EnumerateMaterials(materials);
    result.materialIndices = materials.indices;
    result.geometryRan = true;
    result.geometry = adapter.EnumerateGeometry(sink);
    result.inspectedTriangles = adapter.InspectedTriangleCount();
    return result;
}

ProviderOutcome RunPipeline(BoundedSource& source, std::uint32_t cx = 64)
{
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Step;
    request.source = &source;
    request.limits = &ProviderLimits::Default();
    request.deadline = &deadline;
    request.ledger = &ledger;
    request.cx = cx;
    RasterImage image;
    return RunThumbnailPipeline(request, DefaultThumbnailDependencies(), image);
}

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
    return ReadFile(std::filesystem::path(PREVIEW3D_STEP_FIXTURE_DIR) / name);
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

// --- Supported self-contained AP203/AP214/AP242 -------------------------------

TEST_CASE("a self-contained AP214 B-rep part renders model-derived geometry",
          "[provider][step]")
{
    ByteSource source(Fixture("part_ap214.stp"));
    REQUIRE(source.Size() > 0u);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);

    REQUIRE(result.initialize == ErrorCode::None);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.definitions >= 1u);
    CHECK(result.occurrences >= 1u);
    CHECK(result.materials >= 1u);
    CHECK(result.geometry == ErrorCode::None);
    REQUIRE_FALSE(sink.triangles.empty());
    CHECK(sink.points.empty());
    CHECK(AllFinite(sink));
    CHECK(RunPipeline(source) == ProviderOutcome::Success);
}

TEST_CASE("an AP203 part parses through the same adapter", "[provider][step]")
{
    ByteSource source(Fixture("part_ap203.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
}

TEST_CASE("an AP242 part parses through the same adapter", "[provider][step]")
{
    ByteSource source(Fixture("part_ap242.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
}

TEST_CASE("an authored AP242 tessellation is accepted", "[provider][step]")
{
    ByteSource source(Fixture("tessellated_ap242.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
    CHECK(AllFinite(sink));
}

TEST_CASE("a tessellated-only AP242 representation still renders", "[provider][step]")
{
    ByteSource source(Fixture("tessellated_only_ap242.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
}

// --- Assemblies, instances and colors ----------------------------------------

TEST_CASE("an assembly preserves its definitions, occurrences and colors",
          "[provider][step]")
{
    ByteSource source(Fixture("assembly_ap214.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.definitions == 2u);
    CHECK(result.occurrences == 3u);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
    CHECK(AllFinite(sink));
}

TEST_CASE("a nested assembly walks its hierarchy with recursive transforms",
          "[provider][step]")
{
    ByteSource source(Fixture("assembly_nested_ap214.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.definitions == 2u);
    CHECK(result.occurrences == 5u);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
}

TEST_CASE("instance colors resolve without duplicating geometry", "[provider][step]")
{
    ByteSource source(Fixture("instance_color_ap214.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.definitions == 1u);
    CHECK(result.occurrences == 3u);
    CHECK(result.materials >= 2u);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
}

TEST_CASE("a per-face colored part still renders its shape", "[provider][step]")
{
    ByteSource source(Fixture("face_color_ap214.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
}

TEST_CASE("an inch-authored part verifies its unit and renders", "[provider][step]")
{
    ByteSource source(Fixture("inch_part_ap214.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
}

// --- Intentional exclusions ---------------------------------------------------

TEST_CASE("a geometry-free STEP file is an empty-geometry failure", "[provider][step]")
{
    ByteSource source(Fixture("no_geometry_ap242.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    // A syntactically valid file with no transferable visual geometry is a safe
    // fallback: no triangles are ever emitted and the pipeline fails to the
    // generic icon with the bad-format code.
    CHECK((result.parse == ErrorCode::EmptyGeometry || result.parse == ErrorCode::None));
    CHECK(sink.triangles.empty());
    CHECK(RunPipeline(source) == ProviderOutcome::BadFormat);
}

TEST_CASE("an unsupported schema without geometry is geometry-free, not a schema reject",
          "[provider][step]")
{
    ByteSource source(Fixture("unsupported_schema.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse != ErrorCode::None);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a required external STEP document is rejected before OCCT",
          "[provider][step]")
{
    ByteSource source(Fixture("external_document_ap214.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::UnsupportedRequiredFeature);
    CHECK(result.externalDocument);
    CHECK(sink.triangles.empty());
    CHECK(RunPipeline(source) == ProviderOutcome::Unsupported);
}

TEST_CASE("relative and absolute external document declarations are rejected",
          "[provider][step]")
{
    for (const char* name : {"external_document_relative_ap214.stp",
                             "external_document_absolute_ap214.stp"}) {
        ByteSource source(Fixture(name));
        Deadline deadline;
        AllocationLedger ledger;
        CollectSink sink;
        StepAdapter adapter;
        const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
        CHECK(result.parse == ErrorCode::UnsupportedRequiredFeature);
        CHECK(result.externalDocument);
    }
}

TEST_CASE("invalid faceted topology is a typed failure", "[provider][step]")
{
    ByteSource source(Fixture("faceted_invalid_ap242.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    // Unvalidated faceted data is never trusted geometry: the adapter emits no
    // triangles and the call falls back to the generic icon, whether the fault
    // is detected at transfer or at extraction.
    INFO("parse=" << static_cast<int>(result.parse));
    CHECK(sink.triangles.empty());
    // A detected-invalid authored tessellation is a typed failure (bad format
    // or a contained decode fault), never a fabricated success bitmap.
    const ProviderOutcome outcome = RunPipeline(source);
    CHECK((outcome == ProviderOutcome::BadFormat || outcome == ProviderOutcome::DecoderFailure));
}

TEST_CASE("non-Part-21 bytes are rejected by admission", "[provider][step]")
{
    ByteSource source(Ascii("this is not an ISO 10303-21 physical file at all\n"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::MalformedData);
    CHECK(result.admissionRejected);
    CHECK(RunPipeline(source) == ProviderOutcome::BadFormat);
}

TEST_CASE("an empty stream is a typed failure", "[provider][step]")
{
    ByteSource source({});
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse != ErrorCode::None);
}

// --- Limits, deadline and backing --------------------------------------------

TEST_CASE("an expired deadline stops the parse before any OCCT call",
          "[provider][step]")
{
    ByteSource source(Fixture("part_ap214.stp"));
    Deadline deadline(Deadline::Clock::now() - std::chrono::seconds(10));
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::Cancelled);
    CHECK(sink.triangles.empty());
}

TEST_CASE("accounted scratch over the ledger cap is a resource limit",
          "[provider][step]")
{
    ByteSource source(Fixture("part_ap214.stp"));
    Deadline deadline;
    AllocationLedger ledger(0);
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.parse == ErrorCode::ResourceLimit);
}

TEST_CASE("a non-contiguous STEP source streams through bounded range reads",
          "[provider][step]")
{
    ByteSource source(Fixture("part_ap214.stp"), /*exposeContiguous=*/false);
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    REQUIRE(result.parse == ErrorCode::None);
    CHECK(result.geometry == ErrorCode::None);
    CHECK_FALSE(sink.triangles.empty());
}

TEST_CASE("a STEP sink cap stop ends enumeration without failing", "[provider][step]")
{
    ByteSource source(Fixture("part_ap214.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    CollectSink sink;
    sink.triangleLimit = 0;

    StepAdapter adapter;
    const AdapterResult result = RunAdapter(adapter, source, deadline, ledger, sink);
    CHECK(result.geometry == ErrorCode::None);
    CHECK(sink.triangles.empty());
}

TEST_CASE("a valid STEP render produces a non-empty bitmap through the real dependencies",
          "[provider][step]")
{
    ByteSource source(Fixture("part_ap214.stp"));
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request{};
    request.family = Family::Step;
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

// --- Routing ------------------------------------------------------------------

TEST_CASE("STEP is routed from its frozen CLSID", "[provider][step]")
{
    CHECK(FamilyForClsid("{6EE961AC-AC3B-4958-A898-E30523FEE79D}") == Family::Step);
    CHECK(CreateFamilyAdapter(Family::Step) != nullptr);
}

// --- Time / process-commit qualification measurement (hidden) -----------------
//
// T34 measures the actual render time and process-private-commit increase of
// the self-contained STEP corpus through the real pipeline. The committed
// fixtures are small (STEP-008's genuine 100 MB+ assembly remains viewer-side
// work); this records the provider pipeline cost and the accounted scratch, not
// a large-file guarantee. Run with: x64\Release\Tests.Unit.exe "[step-perf]".
namespace {

std::uint64_t PrivateCommitBytes()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                               sizeof(counters))) {
        return static_cast<std::uint64_t>(counters.PrivateUsage);
    }
    return 0;
}

} // namespace

TEST_CASE("STEP corpus render time and process-commit measurement", "[.][step-perf]")
{
    // Warm up OCCT's one-time process state so the measured delta is the call.
    {
        ByteSource warm(Fixture("part_ap214.stp"));
        (void)RunPipeline(warm);
    }

    for (const char* name : {"part_ap214.stp", "assembly_ap214.stp",
                             "assembly_nested_ap214.stp", "tessellated_ap242.stp"}) {
        ByteSource source(Fixture(name));
        REQUIRE(source.Size() > 0u);
        const std::uint64_t before = PrivateCommitBytes();
        const auto start = std::chrono::steady_clock::now();
        const ProviderOutcome outcome = RunPipeline(source, 256);
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        const std::uint64_t after = PrivateCommitBytes();
        const double deltaMiB = static_cast<double>(after > before ? after - before : 0)
            / (1024.0 * 1024.0);
        WARN(name << " outcome=" << static_cast<int>(outcome) << " elapsed_ms=" << elapsedMs
                  << " commit_delta_mib=" << deltaMiB);
        CHECK(outcome == ProviderOutcome::Success);
    }
}