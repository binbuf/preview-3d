// T14 deterministic geometry sampler coverage.
//
// The real sampler and the over-cap policy are compiled directly into
// Tests.Unit.exe (GeometrySampler.cpp), so these exercise the shipped code:
//
//   - reordering determinism: a reversed or shuffled enumeration yields the
//     same retained set and the same byte order, including over the cap;
//   - caps: enumeration stops at the frozen inspect caps and never retains more
//     than the rasterized-sample cap;
//   - coverage: a separated component and a distinct material survive the cap;
//   - validation: NaN/Inf and degenerate samples are discarded and bounds stay
//     finite;
//   - policy: the over-cap decision table is deterministic and never a prefix.

#include <catch2/catch_test_macros.hpp>

#include "AllocationLedger.h"
#include "DeterministicGeometrySampler.h"
#include "GeometrySamplingPolicy.h"
#include "ProviderLimits.h"
#include "ThumbnailPipeline.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using namespace preview3d::provider;

namespace {

TriangleSample MakeTriangle(double x, double y, double z, std::uint32_t material = 1,
                            std::uint32_t reserved = 0)
{
    TriangleSample triangle{};
    triangle.vertices[0].position[0] = 0.0f;
    triangle.vertices[0].position[1] = 0.0f;
    triangle.vertices[0].position[2] = 0.0f;
    triangle.vertices[1].position[0] = 0.5f;
    triangle.vertices[1].position[1] = 0.0f;
    triangle.vertices[1].position[2] = 0.0f;
    triangle.vertices[2].position[0] = 0.0f;
    triangle.vertices[2].position[1] = 0.5f;
    triangle.vertices[2].position[2] = 0.0f;
    triangle.origin[0] = x;
    triangle.origin[1] = y;
    triangle.origin[2] = z;
    triangle.materialIndex = material;
    triangle.reserved = reserved;
    return triangle;
}

PointSample MakePoint(double x, double y, double z, std::uint32_t material = 1)
{
    PointSample point{};
    point.vertex.position[0] = 0.0f;
    point.vertex.position[1] = 0.0f;
    point.vertex.position[2] = 0.0f;
    point.origin[0] = x;
    point.origin[1] = y;
    point.origin[2] = z;
    point.materialIndex = material;
    return point;
}

bool SameTriangle(const TriangleSample& a, const TriangleSample& b)
{
    if (a.materialIndex != b.materialIndex || a.reserved != b.reserved) {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (a.origin[axis] != b.origin[axis]) {
            return false;
        }
    }
    for (int vertex = 0; vertex < 3; ++vertex) {
        for (int axis = 0; axis < 3; ++axis) {
            if (a.vertices[vertex].position[axis] !=
                    b.vertices[vertex].position[axis] ||
                a.vertices[vertex].normal[axis] !=
                    b.vertices[vertex].normal[axis]) {
                return false;
            }
        }
        for (int channel = 0; channel < 4; ++channel) {
            if (a.vertices[vertex].color[channel] !=
                b.vertices[vertex].color[channel]) {
                return false;
            }
        }
    }
    return true;
}

bool SamePoint(const PointSample& a, const PointSample& b)
{
    if (a.materialIndex != b.materialIndex || a.reserved != b.reserved ||
        a.radius != b.radius) {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (a.origin[axis] != b.origin[axis] ||
            a.vertex.position[axis] != b.vertex.position[axis]) {
            return false;
        }
    }
    return true;
}

bool ResultsIdentical(const DeterministicGeometrySampler& a,
                      const DeterministicGeometrySampler& b)
{
    const SampledGeometry& left = a.Result();
    const SampledGeometry& right = b.Result();
    if (left.triangles.size() != right.triangles.size() ||
        left.points.size() != right.points.size() ||
        left.bounds.valid != right.bounds.valid) {
        return false;
    }
    for (std::size_t i = 0; i < left.triangles.size(); ++i) {
        if (!SameTriangle(left.triangles[i], right.triangles[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < left.points.size(); ++i) {
        if (!SamePoint(left.points[i], right.points[i])) {
            return false;
        }
    }
    return true;
}

// A deterministic stream of distinct triangles clustered inside one spatial
// cell; `count` is chosen over the retained cap in the coverage test.
std::vector<TriangleSample> MakeCluster(std::uint64_t count,
                                        std::uint32_t material = 1)
{
    std::vector<TriangleSample> samples;
    samples.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        const double jitter = static_cast<double>(i) * 1e-6;
        samples.push_back(MakeTriangle(jitter, jitter, 0.0, material));
    }
    return samples;
}

} // namespace

TEST_CASE("the sampler discards non-finite and degenerate samples",
          "[provider][sampler]")
{
    DeterministicGeometrySampler sampler(nullptr);
    sampler.Begin(1234);

    TriangleSample nanTriangle = MakeTriangle(0.0, 0.0, 0.0);
    nanTriangle.vertices[1].position[0] =
        (std::numeric_limits<float>::quiet_NaN)();
    CHECK(sampler.AddTriangle(nanTriangle));

    TriangleSample infTriangle = MakeTriangle(0.0, 0.0, 0.0);
    infTriangle.origin[0] = (std::numeric_limits<double>::infinity)();
    CHECK(sampler.AddTriangle(infTriangle));

    TriangleSample degenerate{};
    CHECK(sampler.AddTriangle(degenerate));

    PointSample nanPoint = MakePoint(0.0, 0.0, 0.0);
    nanPoint.origin[1] = (std::numeric_limits<double>::quiet_NaN)();
    CHECK(sampler.AddPoint(nanPoint));

    CHECK(sampler.InspectedTriangles() == 3u);
    CHECK(sampler.InspectedPoints() == 1u);
    CHECK(sampler.DroppedNonFinite() == 3u);
    CHECK(sampler.DroppedDegenerate() == 1u);

    const SampledGeometry& result = sampler.Result();
    CHECK(result.triangles.empty());
    CHECK(result.points.empty());
    CHECK_FALSE(result.bounds.valid);
}

TEST_CASE("the sampler accumulates finite bounds", "[provider][sampler]")
{
    DeterministicGeometrySampler sampler(nullptr);
    sampler.Begin(7);

    CHECK(sampler.AddTriangle(MakeTriangle(-2.0, 1.0, -3.0)));
    CHECK(sampler.AddPoint(MakePoint(4.0, -5.0, 0.5)));

    const SampledGeometry& result = sampler.Result();
    REQUIRE(result.bounds.valid);
    CHECK(result.bounds.min[0] == -2.0);
    CHECK(result.bounds.min[1] == -5.0);
    CHECK(result.bounds.min[2] == -3.0);
    // Triangle local vertices reach +0.5 in x/y from its origin.
    CHECK(result.bounds.max[0] == 4.0);
    CHECK(result.bounds.max[1] == 1.5);
    CHECK(result.bounds.max[2] == 0.5);
}

TEST_CASE("a reordered enumeration yields the same selection", "[provider][sampler]")
{
    constexpr std::uint64_t kCount = 4096;

    std::vector<TriangleSample> samples;
    samples.reserve(kCount);
    for (std::uint64_t i = 0; i < kCount; ++i) {
        samples.push_back(MakeTriangle(static_cast<double>(i) * 0.25,
                                       static_cast<double>(i % 37),
                                       static_cast<double>(i % 11), 1));
    }

    DeterministicGeometrySampler forward(nullptr);
    DeterministicGeometrySampler reversed(nullptr);
    forward.Begin(99);
    reversed.Begin(99);
    for (const TriangleSample& triangle : samples) {
        CHECK(forward.AddTriangle(triangle));
    }
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
        CHECK(reversed.AddTriangle(*it));
    }

    // Every sample fits the cap, so both retain the whole set in key order.
    CHECK(forward.Result().triangles.size() == kCount);
    CHECK(ResultsIdentical(forward, reversed));

    DeterministicGeometrySampler again(nullptr);
    again.Begin(99);
    for (const TriangleSample& triangle : samples) {
        again.AddTriangle(triangle);
    }
    CHECK(ResultsIdentical(forward, again));
}

TEST_CASE("a reordered enumeration over the cap is byte-identical",
          "[provider][sampler]")
{
    const std::uint64_t count = DeterministicGeometrySampler::kMaxRetained + 4096;
    std::vector<TriangleSample> samples = MakeCluster(count);

    DeterministicGeometrySampler forward(nullptr);
    DeterministicGeometrySampler reversed(nullptr);
    forward.Begin(0xC0FFEEull);
    reversed.Begin(0xC0FFEEull);
    for (const TriangleSample& triangle : samples) {
        forward.AddTriangle(triangle);
    }
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
        reversed.AddTriangle(*it);
    }

    const SampledGeometry& left = forward.Result();
    const SampledGeometry& right = reversed.Result();
    CHECK(left.triangles.size() <= DeterministicGeometrySampler::kMaxRetained);
    CHECK(left.triangles.size() == right.triangles.size());
    CHECK(left.triangles.size() < count);
    CHECK(ResultsIdentical(forward, reversed));
}

TEST_CASE("a separated component and a distinct material survive the cap",
          "[provider][sampler]")
{
    const std::uint64_t count = DeterministicGeometrySampler::kMaxRetained + 2048;
    const std::vector<TriangleSample> cluster = MakeCluster(count, 1);

    DeterministicGeometrySampler sampler(nullptr);
    sampler.Begin(42);
    for (const TriangleSample& triangle : cluster) {
        sampler.AddTriangle(triangle);
    }

    // A spatially isolated component far outside the cluster's cell.
    const TriangleSample farComponent = MakeTriangle(1000.0, 1000.0, 1000.0, 1);
    CHECK(sampler.AddTriangle(farComponent));
    // A distinct material near the cluster; only the material stratum protects
    // it once the priority reservoir is full.
    const TriangleSample otherMaterial =
        MakeTriangle(0.125, 0.125, 0.0, 2, 77);
    CHECK(sampler.AddTriangle(otherMaterial));

    const SampledGeometry& result = sampler.Result();
    CHECK(result.triangles.size() <= DeterministicGeometrySampler::kMaxRetained);

    bool farFound = false;
    bool materialFound = false;
    for (const TriangleSample& triangle : result.triangles) {
        if (triangle.origin[0] == 1000.0 && triangle.origin[1] == 1000.0) {
            farFound = true;
        }
        if (triangle.materialIndex == 2) {
            materialFound = true;
        }
    }
    CHECK(farFound);
    CHECK(materialFound);
}

TEST_CASE("enumeration stops at the frozen inspect caps", "[provider][sampler]")
{
    DeterministicGeometrySampler sampler(nullptr);
    sampler.Begin(5);

    // The stream is fed past the cap; the call at the cap is accepted and the
    // next returns false, so an adapter stops exactly at the cap (never a
    // source prefix while under it).
    std::uint64_t acceptedTriangles = 0;
    bool triangleStopped = false;
    for (std::uint64_t i = 0; i < ProviderLimits::kTrianglesInspectedMax + 8; ++i) {
        if (!sampler.AddTriangle(MakeTriangle(0.0, 0.0, 0.0))) {
            triangleStopped = true;
            break;
        }
        ++acceptedTriangles;
    }
    CHECK(triangleStopped);
    CHECK(acceptedTriangles == ProviderLimits::kTrianglesInspectedMax);
    CHECK(sampler.TrianglesCapReached());
    CHECK(sampler.InspectedTriangles() == ProviderLimits::kTrianglesInspectedMax);

    std::uint64_t acceptedPoints = 0;
    bool pointStopped = false;
    for (std::uint64_t i = 0; i < ProviderLimits::kPointsInspectedMax + 8; ++i) {
        if (!sampler.AddPoint(MakePoint(0.0, 0.0, 0.0))) {
            pointStopped = true;
            break;
        }
        ++acceptedPoints;
    }
    CHECK(pointStopped);
    CHECK(acceptedPoints == ProviderLimits::kPointsInspectedMax);
    CHECK(sampler.PointsCapReached());
    CHECK(sampler.InspectedPoints() == ProviderLimits::kPointsInspectedMax);
}

TEST_CASE("the sampler never retains more than the rasterized cap",
          "[provider][sampler]")
{
    const std::vector<TriangleSample> samples =
        MakeCluster(DeterministicGeometrySampler::kMaxRetained + 1024);
    DeterministicGeometrySampler sampler(nullptr);
    sampler.Begin(11);
    for (const TriangleSample& triangle : samples) {
        sampler.AddTriangle(triangle);
    }
    CHECK(sampler.Result().triangles.size() <=
          DeterministicGeometrySampler::kMaxRetained);
    CHECK(sampler.RetainedSamples() <= DeterministicGeometrySampler::kMaxRetained);
}

TEST_CASE("a full ledger degrades to an empty, non-crashing result",
          "[provider][sampler]")
{
    AllocationLedger tiny(1);
    DeterministicGeometrySampler sampler(&tiny);
    sampler.Begin(3);
    CHECK(sampler.AccountingFailed());
    CHECK(sampler.AddTriangle(MakeTriangle(0.0, 0.0, 0.0)));
    CHECK(sampler.Result().triangles.empty());
}

TEST_CASE("the over-cap policy never renders a biased prefix",
          "[provider][sampler][policy]")
{
    const std::uint64_t cap = ProviderLimits::kTrianglesInspectedMax;

    const GeometrySamplingDecision within =
        DecideGeometrySampling(false, cap, cap);
    CHECK(within.plan == GeometrySamplingPlan::Enumerate);
    CHECK(within.inspectCap == cap);
    CHECK(within.strata == 0u);

    const GeometrySamplingDecision seekable =
        DecideGeometrySampling(true, cap + 1, cap);
    CHECK(seekable.plan == GeometrySamplingPlan::StratifiedAcrossExtent);
    CHECK(seekable.strata == kGeometrySamplingStrata);

    const GeometrySamplingDecision notSeekable =
        DecideGeometrySampling(false, cap + 1, cap);
    CHECK(notSeekable.plan == GeometrySamplingPlan::SafeFallback);
    CHECK(notSeekable.strata == 0u);
}

TEST_CASE("stratified offsets spread across the whole extent",
          "[provider][sampler][policy]")
{
    const std::vector<std::uint64_t> offsets = StratifiedOffsets(1000, 64);
    REQUIRE(offsets.size() == 64u);
    CHECK(offsets.front() > 0u);
    CHECK(offsets.back() < 1000u);
    for (std::size_t i = 1; i < offsets.size(); ++i) {
        CHECK(offsets[i] > offsets[i - 1]);
    }
    // Stable for the same (count, strata) and never all-prefix.
    CHECK(StratifiedOffsets(1000, 64) == offsets);
    CHECK(StratifiedOffsets(5, 0).empty());
    CHECK(StratifiedOffsets(0, 8).empty());
    CHECK(StratifiedOffsets(3, 8).size() == 3u);
}

TEST_CASE("the production dependencies expose the real sampler",
          "[provider][sampler][pipeline]")
{
    IThumbnailDependencies& dependencies = DefaultThumbnailDependencies();
    std::unique_ptr<IGeometrySampler> sampler = dependencies.CreateSampler();
    REQUIRE(sampler != nullptr);
    sampler->Begin(1);
    CHECK(sampler->AddTriangle(MakeTriangle(0.0, 0.0, 0.0)));
    CHECK_FALSE(sampler->Result().triangles.empty());
}