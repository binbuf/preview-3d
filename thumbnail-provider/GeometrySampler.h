#pragma once

// Frozen geometry-sampler contract (T04). The sampler reduces an arbitrarily
// large inspected mesh/point set to a small, deterministic representative set
// that preserves silhouette, material boundaries and disconnected components
// within the provider's sampled caps (design/05, "Geometry sampling"; T14
// implements it).
//
// The sampler owns the sampled storage for the duration of one GetThumbnail
// call and charges it against the T06 process-wide ledger. It inspects at most
// the T06 caps and admits at most the T06 rasterized-sample cap; the seed is
// derived from the source so Explorer's cache stays consistent across runs.

#include "ProviderTypes.h"

#include <cstdint>
#include <span>

namespace preview3d::provider {

// The bounded representative set handed to the rasterizer. Spans are valid
// until the sampler is reset or destroyed and never outlive the call.
struct SampledGeometry {
    std::span<const TriangleSample> triangles;
    std::span<const PointSample> points;
    Bounds bounds;
};

class IGeometrySampler {
public:
    IGeometrySampler() = default;
    IGeometrySampler(const IGeometrySampler&) = delete;
    IGeometrySampler& operator=(const IGeometrySampler&) = delete;
    virtual ~IGeometrySampler() noexcept = default;

    // Starts a sampling call. `sourceSeed` must be stable for a given source so
    // the selection is reproducible; callers pass a hash of the validated
    // source bytes/identity. Resets any previous result.
    virtual void Begin(std::uint64_t sourceSeed) noexcept = 0;

    // Feeds one inspected triangle/point. Returns false when the inspect cap or
    // the rasterized-sample cap is reached; the adapter stops enumerating.
    virtual bool AddTriangle(const TriangleSample& triangle) noexcept = 0;
    virtual bool AddPoint(const PointSample& point) noexcept = 0;

    // The deterministic representative set after enumeration. Empty until at
    // least one sample is admitted; bounds are valid only when the result is
    // nonempty.
    virtual const SampledGeometry& Result() const noexcept = 0;
};

} // namespace preview3d::provider