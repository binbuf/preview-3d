#pragma once

// T14 over-cap sampling policy (design/05-thumbnail-provider.md, "Geometry
// sampling"; ADR-0016). The frozen `IGeometrySampler` interface is stream-only:
// it never opens the source, so it cannot place strata across a declared extent
// itself. The adapters (T21-T34) own the source I/O, but the *policy* for what
// to do when a source exceeds the 2M-triangle / 6M-point inspect cap is fixed
// here, in one place, so every family behaves the same way.
//
// Rules (design/05):
//   - a source within the inspect cap is enumerated in full;
//   - an over-cap source on a seek-capable stream is sampled as deterministic
//     strata across the whole declared extent, never a source prefix;
//   - an over-cap source the provider cannot seek is the safe generic-icon
//     fallback rather than a biased source prefix. Trustworthy metadata alone
//     cannot make a non-seekable stream seekable, so it does not change the
//     plan (ADR-0016).
//
// `StratifiedOffsets` is the deterministic offset set an adapter uses to place
// those strata: `strata` evenly spread record offsets across `count`, stable
// for a given (count, strata) and therefore across reordered enumeration.

#include "ProviderLimits.h"

#include <cstdint>
#include <vector>

namespace preview3d::provider {

// How an adapter should enumerate geometry for one routed source.
enum class GeometrySamplingPlan : std::uint32_t {
    // The declared primitive count fits the inspect cap: stream the source in
    // order into the sampler (which is itself source-order independent).
    Enumerate = 0,
    // Over the inspect cap on a seek-capable stream: read the deterministic
    // stratified offsets across the declared extent.
    StratifiedAcrossExtent,
    // Over the inspect cap on a stream that cannot be positioned: fail to the
    // generic icon instead of rendering a biased source prefix.
    SafeFallback,
};

// The number of spatial strata an over-cap adapter places across the extent.
// Bounded so the offset set stays tiny next to the inspect cap.
inline constexpr std::uint32_t kGeometrySamplingStrata = 64;

struct GeometrySamplingDecision {
    GeometrySamplingPlan plan = GeometrySamplingPlan::Enumerate;
    // The cap the decision was made against (the matching triangle or point
    // inspect cap), so the adapter does not repeat the comparison.
    std::uint64_t inspectCap = 0;
    // Nonzero only for StratifiedAcrossExtent: how many strata to read.
    std::uint32_t strata = 0;
};

// The single over-cap policy. `declaredPrimitives` is what the source declares
// (or what the adapter can bound); `inspectCap` is the triangle or point cap
// from `ProviderLimits` for the primitive kind being enumerated.
constexpr GeometrySamplingDecision DecideGeometrySampling(
    bool seekable, std::uint64_t declaredPrimitives,
    std::uint64_t inspectCap) noexcept
{
    GeometrySamplingDecision decision{};
    decision.inspectCap = inspectCap;
    if (declaredPrimitives <= inspectCap) {
        decision.plan = GeometrySamplingPlan::Enumerate;
        return decision;
    }
    if (seekable) {
        decision.plan = GeometrySamplingPlan::StratifiedAcrossExtent;
        decision.strata = kGeometrySamplingStrata;
        return decision;
    }
    decision.plan = GeometrySamplingPlan::SafeFallback;
    return decision;
}

// Deterministic evenly spread offsets across `count` records, one per stratum,
// sorted and unique. The adapter reads a small window at each offset and feeds
// the sampler; because the offsets depend only on (count, strata) the selection
// is identical no matter which end of the source the adapter started from.
std::vector<std::uint64_t> StratifiedOffsets(std::uint64_t count,
                                             std::uint32_t strata) noexcept;

} // namespace preview3d::provider