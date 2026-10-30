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
// v4: preserve double-precision placement before float normalization, emit
// curved-face normals, honor XDE visibility/face colors, and mark reusable
// geometry as instance-only sources.
// v5: lower the minimum meshing edge to preserve small valid STEP features.
// v6 (T07): Draft/Balanced/Full import quality (Balanced keeps the v5 display
// values), plus the opt-in bounded repair policy that can change emitted
// geometry for a file whose definitions fail to tessellate.
constexpr std::uint32_t kStepTessellationProfileVersion = 6;

// The pre-T07 names remain as aliases of the Balanced/Draft mapping so existing
// callers and tests keep their meaning: Coarse == Draft, Display == Balanced.
enum class StepTessellationQuality : std::uint32_t {
    Draft = 0,
    Balanced = 1,
    Full = 2,
    Coarse = Draft,
    Display = Balanced,
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
    // Full is only reachable through an explicit import-quality request. It
    // stays inside the same absolute window and all per-definition budgets, so
    // a finer request cannot turn a bounded file into an unbounded one.
    double fullRelativeDeflection = 0.001;
    // Absolute linear-deflection window in transferred (Cascade) length units.
    double minAbsoluteDeflection = 0.001; // 1 micrometre (mm space)
    double maxAbsoluteDeflection = 5.0;   // 5 millimetres (mm space)
    // Angular chord deflection in radians.
    double coarseAngularDeflection = 0.6;
    double displayAngularDeflection = 0.25;
    double fullAngularDeflection = 0.12;
    // Minimum triangle edge as a fraction of the diagonal, clamped absolutely
    // so distorted surfaces cannot amplify into unbounded tessellation.
    double relativeMinEdge = 0.0001;
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
        switch (quality) {
        case StepTessellationQuality::Draft: return coarseRelativeDeflection;
        case StepTessellationQuality::Full: return fullRelativeDeflection;
        case StepTessellationQuality::Balanced: break;
        }
        return displayRelativeDeflection;
    }
    double AngularDeflection(StepTessellationQuality quality) const noexcept
    {
        switch (quality) {
        case StepTessellationQuality::Draft: return coarseAngularDeflection;
        case StepTessellationQuality::Full: return fullAngularDeflection;
        case StepTessellationQuality::Balanced: break;
        }
        return displayAngularDeflection;
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
// T07 adopts the narrow opt-in path permitted by that decision: a bounded,
// deterministic `ShapeFix_Shape` pass that runs only after a definition has
// transferred but produced no tessellation. It never runs on a definition that
// meshed successfully, never raises a per-definition budget, and if it cannot
// produce a valid shape the original typed outcome is preserved. A successful
// repair is reported as `Degraded` and bumps the profile version because the
// emitted bytes can differ from the no-repair path.
enum class StepHealingPolicy : std::uint32_t {
    None = 0,
    BoundedRepairOnFailure = 1,
};
constexpr StepHealingPolicy kStepHealingPolicy =
    StepHealingPolicy::BoundedRepairOnFailure;

// Hard pre/post bounds for the repair pass. The face/edge counts are checked
// before entering ShapeFix; the elapsed check after `Perform` rejects a result
// that took too long (ShapeFix has no cancellable progress range). The repair
// is per definition, so the backstop stays the existing per-generation limits.
struct StepRepairBudget {
    std::uint32_t maxFaces = 20'000;
    std::uint32_t maxEdges = 200'000;
    double maxMilliseconds = 30'000.0;
};

// T07 length-unit policy. The pinned reader normalizes transferred coordinates
// to the Cascade system unit; this resolves the reported metre factor into the
// product's display assumption without ever claiming an authored unit that was
// not present.
//
//   * zero authored length units, or a non-finite/non-positive reported factor,
//     means "assume millimetres" (0.001 m per transferred unit);
//   * more than one distinct authored unit is contradictory: the reader's
//     first/dominant factor is used and `contradictory` is reported;
//   * a positive factor above the validated bound is retained so the caller can
//     reject it with the existing typed overflow failure.
struct StepLengthUnitResolution {
    double metersPerUnit = 0.001;
    bool assumedMillimetres = false;
    bool contradictory = false;
    bool overflow = false;
};

inline StepLengthUnitResolution ResolveStepLengthUnit(
    std::uint32_t distinctAuthoredUnits, double reportedMetersPerUnit) noexcept
{
    StepLengthUnitResolution resolution;
    resolution.contradictory = distinctAuthoredUnits > 1;
    const bool reportedUsable = std::isfinite(reportedMetersPerUnit)
        && reportedMetersPerUnit > 0.0;
    if (distinctAuthoredUnits == 0 || !reportedUsable) {
        resolution.assumedMillimetres = true;
        resolution.metersPerUnit = 0.001;
        return resolution;
    }
    resolution.metersPerUnit = reportedMetersPerUnit;
    resolution.overflow = reportedMetersPerUnit > 1e12;
    return resolution;
}

} // namespace step_host
