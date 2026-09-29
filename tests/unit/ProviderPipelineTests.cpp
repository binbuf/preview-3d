// T13 routed thumbnail pipeline coverage.
//
// RunThumbnailPipeline and its IThumbnailDependencies seam are compiled directly
// into Tests.Unit.exe (ThumbnailPipeline.cpp), so the real orchestration is
// exercised with deterministic doubles:
//
//   - routing: every frozen family route reaches its adapter and the adapter
//     receives the CLSID-selected family plus the bounded source/limits/deadline;
//   - error mapping: a failure at each adapter stage and each taxonomy class
//     returns the exact tabulated HRESULT and never a fabricated image;
//   - success: materials and sampled geometry flow into the rasterizer, and the
//     adapter is always Reset.
//
// The COM wrapper (GetThumbnail) and the DIB boundary are covered separately by
// ProviderThumbnailTests.cpp and ProviderRasterBitmapTests.cpp.

#include <catch2/catch_test_macros.hpp>

#include "AllocationLedger.h"
#include "Deadline.h"
#include "FamilyRouting.h"
#include "ProviderErrors.h"
#include "ProviderLimits.h"
#include "ThumbnailPipeline.h"

#include "model_core/MaterialPayload.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

using namespace preview3d::provider;

namespace {

// A minimal BoundedSource double; the pipeline never reads it, it only seeds the
// sampler from its validated size.
class MemorySource final : public BoundedSource {
public:
    explicit MemorySource(std::uint64_t size) noexcept : size_(size) {}
    std::uint64_t Size() const noexcept override { return size_; }
    bool Seekable() const noexcept override { return true; }
    bool ReadAt(std::uint64_t, std::span<std::byte>) override { return false; }
    std::span<const std::byte> ContiguousView() override { return {}; }

private:
    std::uint64_t size_;
};

// Outlives every ScriptedAdapter instance the dependencies hand to the pipeline.
struct AdapterState {
    ErrorCode initializeResult = ErrorCode::None;
    ErrorCode parseResult = ErrorCode::None;
    ErrorCode materialsResult = ErrorCode::None;
    ErrorCode geometryResult = ErrorCode::None;
    int triangleCount = 0;
    int pointCount = 0;
    int materialCount = 0;
    int initializeCalls = 0;
    bool resetCalled = false;
    AdapterInput seenInput{};
};

class ScriptedAdapter final : public IFamilyAdapter {
public:
    explicit ScriptedAdapter(AdapterState* state) noexcept : state_(state) {}

    ErrorCode Initialize(const AdapterInput& input) noexcept override
    {
        ++state_->initializeCalls;
        state_->seenInput = input;
        return state_->initializeResult;
    }

    ErrorCode Parse() noexcept override { return state_->parseResult; }

    ErrorCode EnumerateMaterials(IMaterialSink& sink) noexcept override
    {
        for (int i = 0; i < state_->materialCount; ++i) {
            if (!sink.OnMaterial(static_cast<std::uint32_t>(i + 1), NeutralMaterial())) {
                break;
            }
        }
        return state_->materialsResult;
    }

    ErrorCode EnumerateGeometry(IGeometrySink& sink) noexcept override
    {
        for (int i = 0; i < state_->triangleCount; ++i) {
            TriangleSample triangle{};
            triangle.vertices[0].position[0] = static_cast<float>(i);
            if (!sink.OnTriangle(triangle)) {
                break;
            }
        }
        for (int i = 0; i < state_->pointCount; ++i) {
            PointSample point{};
            point.vertex.position[1] = static_cast<float>(i);
            if (!sink.OnPoint(point)) {
                break;
            }
        }
        return state_->geometryResult;
    }

    void Reset() noexcept override { state_->resetCalled = true; }

private:
    AdapterState* state_;
};

struct SamplerState {
    std::vector<TriangleSample> triangles;
    std::vector<PointSample> points;
    std::uint64_t seed = 0;
    bool begun = false;
};

class ScriptedSampler final : public IGeometrySampler {
public:
    explicit ScriptedSampler(SamplerState* state) noexcept : state_(state) {}

    void Begin(std::uint64_t sourceSeed) noexcept override
    {
        state_->seed = sourceSeed;
        state_->begun = true;
        state_->triangles.clear();
        state_->points.clear();
        result_ = SampledGeometry{};
    }

    bool AddTriangle(const TriangleSample& triangle) noexcept override
    {
        state_->triangles.push_back(triangle);
        return true;
    }

    bool AddPoint(const PointSample& point) noexcept override
    {
        state_->points.push_back(point);
        return true;
    }

    const SampledGeometry& Result() const noexcept override
    {
        result_.triangles = state_->triangles;
        result_.points = state_->points;
        result_.bounds.valid = !state_->triangles.empty() || !state_->points.empty();
        return result_;
    }

private:
    SamplerState* state_;
    mutable SampledGeometry result_{};
};

class ScriptedDependencies final : public IThumbnailDependencies {
public:
    AdapterState adapter;
    SamplerState sampler;
    bool returnAdapter = true;
    bool returnSampler = true;
    ErrorCode renderResult = ErrorCode::None;
    RasterImage renderImage;
    int renderCalls = 0;
    Family requestedFamily = Family::Unknown;
    std::uint32_t seenRequestedSize = 0;
    std::size_t seenMaterialCount = 0;
    std::size_t seenTriangleCount = 0;
    std::size_t seenPointCount = 0;

    std::unique_ptr<IFamilyAdapter> CreateAdapter(Family family) noexcept override
    {
        requestedFamily = family;
        if (!returnAdapter) {
            return nullptr;
        }
        return std::make_unique<ScriptedAdapter>(&adapter);
    }

    std::unique_ptr<IGeometrySampler> CreateSampler() noexcept override
    {
        if (!returnSampler) {
            return nullptr;
        }
        return std::make_unique<ScriptedSampler>(&sampler);
    }

    ErrorCode Render(const RasterRequest& request, RasterImage& out) noexcept override
    {
        ++renderCalls;
        seenRequestedSize = request.requestedSize;
        seenMaterialCount = request.materials.size();
        seenTriangleCount = request.geometry ? request.geometry->triangles.size() : 0;
        seenPointCount = request.geometry ? request.geometry->points.size() : 0;
        if (renderResult != ErrorCode::None) {
            out = RasterImage{};
            return renderResult;
        }
        out = renderImage;
        return ErrorCode::None;
    }
};

RasterImage MakeImage(std::uint32_t width, std::uint32_t height)
{
    RasterImage image;
    image.width = width;
    image.height = height;
    image.bgraPremultiplied.assign(
        static_cast<std::size_t>(width) * height * 4u, std::uint8_t{0x40});
    return image;
}

bool RasterIsEmpty(const RasterImage& image)
{
    return image.width == 0 && image.height == 0 && image.bgraPremultiplied.empty();
}

ThumbnailRequest MakeRequest(Family family, BoundedSource& source, Deadline& deadline,
                             AllocationLedger& ledger, std::uint32_t cx = 64)
{
    ThumbnailRequest request{};
    request.family = family;
    request.source = &source;
    request.limits = &ProviderLimits::Default();
    request.deadline = &deadline;
    request.ledger = &ledger;
    request.cx = cx;
    return request;
}

// The six tabulated rows an adapter failure can land on.
const ErrorCode kMappingCodes[] = {
    ErrorCode::UnsupportedFormat,
    ErrorCode::MalformedData,
    ErrorCode::ResourceLimit,
    ErrorCode::WorkerTimedOut,
    ErrorCode::OutOfMemory,
    ErrorCode::InternalImporterFailure,
};

} // namespace

TEST_CASE("the pipeline routes each frozen family to its adapter", "[provider][pipeline]")
{
    for (const FamilyRoute& route : FamilyRoutes()) {
        ScriptedDependencies dependencies;
        dependencies.adapter.triangleCount = 1;
        dependencies.renderImage = MakeImage(4, 4);

        MemorySource source(4096);
        Deadline deadline;
        AllocationLedger ledger;
        ThumbnailRequest request = MakeRequest(route.family, source, deadline, ledger);
        RasterImage out;

        const ProviderOutcome outcome =
            RunThumbnailPipeline(request, dependencies, out);

        INFO("family CLSID " << route.clsid);
        CHECK(outcome == ProviderOutcome::Success);
        CHECK(dependencies.requestedFamily == route.family);
        CHECK(dependencies.adapter.initializeCalls == 1);
        CHECK(dependencies.adapter.seenInput.family == route.family);
        CHECK(dependencies.adapter.seenInput.source == &source);
        CHECK(dependencies.adapter.seenInput.limits == &ProviderLimits::Default());
        CHECK(dependencies.adapter.seenInput.deadline == &deadline);
        CHECK(dependencies.adapter.seenInput.ledger == &ledger);
        CHECK(dependencies.adapter.resetCalled);
        CHECK_FALSE(RasterIsEmpty(out));
    }
}

TEST_CASE("each adapter stage failure maps to the tabulated HRESULT",
          "[provider][pipeline]")
{
    // stage: 0 Initialize, 1 Parse, 2 EnumerateMaterials, 3 EnumerateGeometry.
    for (int stage = 0; stage < 4; ++stage) {
        for (const ErrorCode code : kMappingCodes) {
            ScriptedDependencies dependencies;
            switch (stage) {
                case 0: dependencies.adapter.initializeResult = code; break;
                case 1: dependencies.adapter.parseResult = code; break;
                case 2: dependencies.adapter.materialsResult = code; break;
                default: dependencies.adapter.geometryResult = code; break;
            }
            dependencies.renderImage = MakeImage(4, 4);

            MemorySource source(4096);
            Deadline deadline;
            AllocationLedger ledger;
            ThumbnailRequest request =
                MakeRequest(Family::Stl, source, deadline, ledger);
            RasterImage out;

            const ProviderOutcome outcome =
                RunThumbnailPipeline(request, dependencies, out);

            INFO("stage " << stage << " code " << static_cast<std::uint32_t>(code));
            CHECK(outcome == ClassifyError(code));
            CHECK(HresultFor(outcome) == HresultForError(code));
            CHECK(RasterIsEmpty(out));
            CHECK(dependencies.renderCalls == 0);
            CHECK(dependencies.adapter.resetCalled);
        }
    }
}

TEST_CASE("a successful adapter flows materials and geometry into the rasterizer",
          "[provider][pipeline]")
{
    ScriptedDependencies dependencies;
    dependencies.adapter.materialCount = 2;
    dependencies.adapter.triangleCount = 3;
    dependencies.adapter.pointCount = 1;
    dependencies.renderImage = MakeImage(8, 8);

    MemorySource source(1 << 20);
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request = MakeRequest(Family::Ply, source, deadline, ledger, 256);
    RasterImage out;

    const ProviderOutcome outcome = RunThumbnailPipeline(request, dependencies, out);

    CHECK(outcome == ProviderOutcome::Success);
    CHECK(dependencies.sampler.begun);
    CHECK(dependencies.sampler.triangles.size() == 3u);
    CHECK(dependencies.sampler.points.size() == 1u);
    CHECK(dependencies.seenMaterialCount == 2u);
    CHECK(dependencies.seenTriangleCount == 3u);
    CHECK(dependencies.seenPointCount == 1u);
    CHECK(dependencies.seenRequestedSize == 256u);
    CHECK_FALSE(RasterIsEmpty(out));
    CHECK(out.width == 8u);
    CHECK(out.height == 8u);
}

TEST_CASE("empty geometry is the bad-format fallback", "[provider][pipeline]")
{
    ScriptedDependencies dependencies;
    MemorySource source(64);
    Deadline deadline;
    AllocationLedger ledger;
    ThumbnailRequest request = MakeRequest(Family::Stl, source, deadline, ledger);
    RasterImage out;

    const ProviderOutcome outcome = RunThumbnailPipeline(request, dependencies, out);

    CHECK(outcome == ProviderOutcome::BadFormat);
    CHECK(RasterIsEmpty(out));
    CHECK(dependencies.renderCalls == 0);
    CHECK(dependencies.adapter.resetCalled);
}

TEST_CASE("a missing linked adapter or sampler is unsupported", "[provider][pipeline]")
{
    MemorySource source(64);
    Deadline deadline;
    AllocationLedger ledger;

    {
        ScriptedDependencies dependencies;
        dependencies.returnAdapter = false;
        RasterImage out;
        ThumbnailRequest request = MakeRequest(Family::Step, source, deadline, ledger);
        CHECK(RunThumbnailPipeline(request, dependencies, out) ==
              ProviderOutcome::Unsupported);
        CHECK(RasterIsEmpty(out));
    }
    {
        ScriptedDependencies dependencies;
        dependencies.returnSampler = false;
        RasterImage out;
        ThumbnailRequest request = MakeRequest(Family::Step, source, deadline, ledger);
        CHECK(RunThumbnailPipeline(request, dependencies, out) ==
              ProviderOutcome::Unsupported);
        CHECK(RasterIsEmpty(out));
        CHECK(dependencies.adapter.resetCalled);
    }
}

TEST_CASE("a failed or empty render never yields a bitmap", "[provider][pipeline]")
{
    MemorySource source(64);
    Deadline deadline;
    AllocationLedger ledger;

    {
        ScriptedDependencies dependencies;
        dependencies.adapter.triangleCount = 1;
        dependencies.renderResult = ErrorCode::MalformedData;
        RasterImage out;
        ThumbnailRequest request = MakeRequest(Family::Stl, source, deadline, ledger);
        CHECK(RunThumbnailPipeline(request, dependencies, out) ==
              ProviderOutcome::BadFormat);
        CHECK(RasterIsEmpty(out));
    }
    {
        ScriptedDependencies dependencies;
        dependencies.adapter.triangleCount = 1;
        // renderImage stays empty: a "success" with no pixels is a decoder fault.
        RasterImage out;
        ThumbnailRequest request = MakeRequest(Family::Stl, source, deadline, ledger);
        CHECK(RunThumbnailPipeline(request, dependencies, out) ==
              ProviderOutcome::DecoderFailure);
        CHECK(RasterIsEmpty(out));
    }
}

TEST_CASE("bad pointers, an expired deadline and a full ledger fail closed",
          "[provider][pipeline]")
{
    MemorySource source(64);
    Deadline deadline;
    AllocationLedger ledger;
    RasterImage out;
    ScriptedDependencies dependencies;

    {
        ThumbnailRequest request = MakeRequest(Family::Stl, source, deadline, ledger);
        request.source = nullptr;
        CHECK(RunThumbnailPipeline(request, dependencies, out) ==
              ProviderOutcome::BadPointer);
    }
    {
        ThumbnailRequest request = MakeRequest(Family::Stl, source, deadline, ledger);
        request.ledger = nullptr;
        CHECK(RunThumbnailPipeline(request, dependencies, out) ==
              ProviderOutcome::BadPointer);
    }
    {
        deadline.Restart(Deadline::Clock::now() - std::chrono::seconds(5));
        ThumbnailRequest request = MakeRequest(Family::Stl, source, deadline, ledger);
        CHECK(RunThumbnailPipeline(request, dependencies, out) ==
              ProviderOutcome::Deadline);
    }
    {
        Deadline fresh;
        AllocationLedger tiny(16);
        dependencies.adapter.triangleCount = 1;
        ThumbnailRequest request = MakeRequest(Family::Stl, source, fresh, tiny);
        CHECK(RunThumbnailPipeline(request, dependencies, out) ==
              ProviderOutcome::LimitExceeded);
        CHECK(RasterIsEmpty(out));
    }
}

TEST_CASE("the source seed is stable per family and size", "[provider][pipeline]")
{
    MemorySource first(4096);
    MemorySource same(4096);
    MemorySource bigger(8192);

    CHECK(SourceSeed(Family::Stl, first) == SourceSeed(Family::Stl, same));
    CHECK(SourceSeed(Family::Stl, first) != SourceSeed(Family::Ply, first));
    CHECK(SourceSeed(Family::Stl, first) != SourceSeed(Family::Stl, bigger));
}