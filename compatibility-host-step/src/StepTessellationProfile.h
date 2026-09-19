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
// v3 (STEP-005): re-enabled multi-threaded OCCT meshing after proving the
// pinned `USE_TBB=OFF` port still has a parallel backend (OSD_Parallel's
// built-in OSD_ThreadPool). See the `parallel` member comment.
constexpr std::uint32_t kStepTessellationProfileVersion = 3;

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
    // One fixed thread policy. STEP-004 assumed `USE_TBB=OFF` meant no parallel
    // `OSD_Parallel` backend and recorded `false`. STEP-005 disproved that:
    // OCCT 7.8's `OSD_Parallel::For` falls back to its own `OSD_ThreadPool`
    // when no external library (TBB) is enabled, and
    // `IMeshTools_Parameters::InParallel` is documented as "switches on/off
    // multi-thread computation". Multi-threaded meshing is therefore available
    // on the pinned port and is enabled here. Per-face triangulation is
    // independent and extraction order is deterministic, so the emitted chunk
    // ids/sizes/checksums are unchanged by thread scheduling; the force-serial
    // test seam proves that identity. This flag is part of the versioned
    // profile, not a host-ambient setting.
    bool parallel = true;
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

// STEP-005 delivery-strategy decision, recorded as a versioned product
// constant rather than a per-file heuristic.
//
// Single-pass progressive display is retained. Two-pass coarse-then-display
// was evaluated and rejected for this slice: STEP is parse/transfer-bound, so a
// second coarse pass cannot make the first geometry appear sooner (nothing is
// emitted until ReadStream/Transfer complete), and the Tier-B broker/bridge do
// not enable the coarse/detail replacement protocol for STEP. The single-pass
// path already streams one bounded window at a time through
// ChunkBatchReady/ChunkBatchConsumed, so time-to-first-usable-frame is the time
// to mesh and emit the first definition, not the whole scene.
enum class StepDeliveryStrategy : std::uint32_t {
    SinglePassProgressiveDisplay = 0,
};
constexpr StepDeliveryStrategy kStepDeliveryStrategy =
    StepDeliveryStrategy::SinglePassProgressiveDisplay;
// Bump only if the chosen strategy changes normalized output. The strategy does
// not participate in kStepImporterVersion while it stays single-pass, because
// declaring the existing behavior does not change any emitted byte.
constexpr std::uint32_t kStepDeliveryStrategyVersion = 1;

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

// STEP-006 healing decision, recorded as a versioned product constant.
//
// The STEP-001 spike used OCCT defaults with no `ShapeFix`/`XSAlgo` sequence.
// STEP-006 compared three options:
//   * no healing (adopted): invalid geometry fails typed instead of being
//     mutated, so the accepted subset is exactly the authored geometry and the
//     visual result is deterministic and producer-independent;
//   * a narrowly pinned validation/healing sequence (rejected for this slice):
//     no measured deterministic visual benefit on the accepted corpus, and it
//     adds a second OCCT algorithm surface with its own failure modes;
//   * broad automatic healing (rejected): silently mutates geometry, makes the
//     result depend on unmeasured OCCT heuristics, and can turn a typed
//     unsupported result into an apparently faithful but altered model.
//
// This does not participate in `kStepImporterVersion`: choosing the existing
// behavior does not change any emitted byte. If a future task adopts a pinned
// healing sequence, it must bump the profile/importer version because output
// could change.
enum class StepHealingPolicy : std::uint32_t {
    None = 0,
};
constexpr StepHealingPolicy kStepHealingPolicy = StepHealingPolicy::None;

} // namespace step_host
