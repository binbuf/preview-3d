#pragma once

// STEP-004 versioned tessellation-quality profile.
//
// The STEP reader transfers exact B-rep data; the product decides every
// tessellation parameter from verified definition bounds, the unit scale, and
// these fixed release-quality constants. They deliberately live outside the
// OCCT-specific translation unit so the arithmetic is unit-testable without
// linking the CAD kernel, and so importer/cache identity can name the exact
// policy that produced a normalized scene.
//
// Design authority: .docs/stp.md (STEP-004). Values are admission/quality
// bounds, never a promise that an OCCT translation of every limit-sized file
// fits; the Tier-B section/Job controls remain the hard backstop.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace step_host {

// Bump on *any* semantic change to the profile or its derivation. This is part
// of the STEP importer/cache identity: two builds that would emit different
// normalized bytes for the same file must not share a cache entry.
//
// v2: corrected the thread-policy member to match the pinned constrained build
// (see below); no output changed, but the recorded policy is now truthful.
constexpr std::uint32_t kStepTessellationProfileVersion = 2;

enum class StepTessellationQuality : std::uint32_t {
    Coarse = 0,
    Display = 1,
};

// Deterministic, model-independent release-quality parameters. The deflection
// values are *relative* to a definition's bounding diagonal and then clamped
// into an absolute window, so a 0.1 mm feature and a 10 m assembly are meshed
// with the same relative fidelity and neither drives an unbounded triangle
// count.
struct StepTessellationProfile {
    // Relative linear deflection (fraction of the definition bounding diagonal).
    double coarseRelativeDeflection = 0.02;
    double displayRelativeDeflection = 0.003;
    // Absolute linear-deflection window in transferred (Cascade) length units.
    double minAbsoluteDeflection = 0.001; // 1 micrometre (mm space)
    double maxAbsoluteDeflection = 5.0;   // 5 millimetres (mm space)
    // Angular chord deflection in radians.
    double coarseAngularDeflection = 0.6;
    double displayAngularDeflection = 0.25;
    // Minimum triangle edge as a fraction of the diagonal, clamped absolutely
    // so distorted surfaces cannot amplify into unbounded tessellation.
    double relativeMinEdge = 0.001;
    double minAbsoluteEdgeLength = 0.001;
    // One fixed thread policy. The pinned constrained OCCT port builds with
    // `USE_TBB=OFF`, so `BRepMesh_IncrementalMesh::InParallel` has no parallel
    // `OSD_Parallel` backend and meshing is serial. STEP-004 records that
    // truthful serial decision; STEP-005 resolves product-level per-definition
    // parallelism (or keeps serial with measured throughput). This flag is part
    // of the versioned profile, not a host-ambient setting.
    bool parallel = false;
    // Per-definition work budgets. Enforced by the adapter before publishing a
    // definition; exceeding one is a typed TessellationFailed/CadKernelLimit,
    // never a silently partial assembly.
    std::uint32_t maxFacesPerDefinition = 200'000;
    std::uint32_t maxEdgesPerDefinition = 2'000'000;
    std::uint32_t maxTrianglesPerDefinition = 8'000'000;
    double maxDefinitionMilliseconds = 120'000.0;
    // Output cluster limit. Must stay inside the broker's per-cluster ceiling
    // (16 MiB / 262,144 triangles); de-indexed vertices make this an upper
    // bound on both vertex and index count.
    std::uint32_t chunkTriangles = 65'536;

    double RelativeDeflection(StepTessellationQuality quality) const noexcept
    {
        return quality == StepTessellationQuality::Coarse ? coarseRelativeDeflection
                                                          : displayRelativeDeflection;
    }
    double AngularDeflection(StepTessellationQuality quality) const noexcept
    {
        return quality == StepTessellationQuality::Coarse ? coarseAngularDeflection
                                                          : displayAngularDeflection;
    }
};

// Derives the absolute linear deflection for a definition whose local
// bounding diagonal is `diagonal` in transferred units. Non-finite or
// non-positive diagonals fall back to the absolute floor (a visibly coarse but
// bounded result) rather than producing an unbounded request.
inline double StepDeriveLinearDeflection(double diagonal, double relative, double minAbsolute,
                                         double maxAbsolute) noexcept
{
    if (!std::isfinite(diagonal) || diagonal <= 0.0) return minAbsolute;
    const double requested = diagonal * relative;
    if (!std::isfinite(requested)) return maxAbsolute;
    return (std::max)(minAbsolute, (std::min)(requested, maxAbsolute));
}

inline double StepDeriveMinEdge(double diagonal, double relativeMinEdge, double minAbsoluteEdge) noexcept
{
    if (!std::isfinite(diagonal) || diagonal <= 0.0) return minAbsoluteEdge;
    const double requested = diagonal * relativeMinEdge;
    if (!std::isfinite(requested)) return minAbsoluteEdge;
    return (std::max)(minAbsoluteEdge, requested);
}

inline bool StepWithinDefinitionTime(double elapsedMilliseconds, double limitMilliseconds) noexcept
{
    return elapsedMilliseconds <= limitMilliseconds;
}

} // namespace step_host